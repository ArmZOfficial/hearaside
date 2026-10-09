-- HEARASIDE accounts (prompt 5B A1). Plain PostgreSQL + RLS so it can move off Supabase later;
-- only auth.users / auth.uid() and the "avatars" storage bucket are Supabase specific.
-- NOT applied to production until the owner approves it (prompt 2 rule 8). Apply with:
--   supabase db push            (linked project)   or   psql "$DATABASE_URL" -f this file

create extension if not exists citext;
create extension if not exists pgcrypto;

-- ---------------------------------------------------------------------------------------------
-- helpers

create or replace function public.touch_updated_at() returns trigger
language plpgsql as $$
begin
  new.updated_at = now();
  return new;
end $$;

-- names nobody may take (links, pages, staff)
create table public.reserved_handles (handle citext primary key);
insert into public.reserved_handles (handle) values
  ('admin'), ('administrator'), ('hearaside'), ('support'), ('help'), ('api'), ('link'), ('login'), ('logout'),
  ('signup'), ('signin'), ('account'), ('accounts'), ('download'), ('downloads'), ('guides'), ('guide'), ('privacy'),
  ('terms'), ('settings'), ('me'), ('root'), ('system'), ('staff'), ('team'), ('official'), ('www'), ('mail'),
  ('l'), ('s'), ('th'), ('en'), ('verify'), ('reset'), ('forgot'), ('null'), ('undefined');
alter table public.reserved_handles enable row level security;   -- no policies: server only

-- ---------------------------------------------------------------------------------------------
-- profiles: one per auth user, created by a trigger; public bits are read through the API only

create table public.profiles (
  id           uuid primary key references auth.users (id) on delete cascade,
  handle       citext not null unique check (handle ~ '^[a-z0-9_]{3,20}$'),
  display_name text   not null default '' check (char_length(display_name) <= 40),
  about        text   not null default '' check (char_length(about) <= 160),
  avatar_path  text,                                    -- <user_id>/<random>.webp in bucket "avatars"
  locale       text   not null default 'en' check (locale in ('en', 'th')),
  hide_email   boolean not null default true,
  plan         text   not null default 'free' check (plan in ('free')),   -- room to grow; no paid plans in this work
  deleted_at   timestamptz,                             -- "Delete account": purged within 30 days
  created_at   timestamptz not null default now(),
  updated_at   timestamptz not null default now()
);
create trigger profiles_touch before update on public.profiles for each row execute function public.touch_updated_at();

create or replace function public.handle_reserved(h citext) returns boolean
language sql stable as $$ select exists (select 1 from public.reserved_handles r where r.handle = h) $$;

alter table public.profiles add constraint profiles_handle_not_reserved check (not public.handle_reserved(handle));

-- a temporary handle from the email (letters / digits only) + 4 random digits; the user changes it later
create or replace function public.new_profile() returns trigger
language plpgsql security definer set search_path = public as $$
declare
  base text := left(regexp_replace(lower(split_part(coalesce(new.email, 'user'), '@', 1)), '[^a-z0-9_]', '', 'g'), 14);
  candidate text;
  tries int := 0;
begin
  if char_length(base) < 3 then base := 'user'; end if;
  loop
    candidate := base || lpad((floor(random() * 10000))::int::text, 4, '0');
    exit when not exists (select 1 from public.profiles p where p.handle = candidate) and not public.handle_reserved(candidate);
    tries := tries + 1;
    if tries > 20 then candidate := 'user' || substr(md5(new.id::text), 1, 12); exit; end if;
  end loop;
  insert into public.profiles (id, handle, display_name, locale)
  values (new.id, candidate,
          left(coalesce(new.raw_user_meta_data ->> 'display_name', new.raw_user_meta_data ->> 'full_name', new.raw_user_meta_data ->> 'name', ''), 40),
          case when new.raw_user_meta_data ->> 'locale' = 'th' then 'th' else 'en' end);
  return new;
end $$;

create trigger on_auth_user_created after insert on auth.users for each row execute function public.new_profile();

alter table public.profiles enable row level security;
create policy "profiles: owner reads"   on public.profiles for select using (id = auth.uid());
create policy "profiles: owner updates" on public.profiles for update using (id = auth.uid()) with check (id = auth.uid() and plan = 'free');

-- ---------------------------------------------------------------------------------------------
-- devices: plug-ins signed in on a computer (device flow, A4). Server only (secret key).

create table public.devices (
  id                 uuid primary key default gen_random_uuid(),
  user_id            uuid not null references auth.users (id) on delete cascade,
  name               text not null default '' check (char_length(name) <= 64),
  os                 text not null default '',
  daw                text not null default '',
  plugin             text not null default '',
  plugin_version     text not null default '',
  refresh_token_hash text not null,          -- SHA-256 (hex) of the current refresh token
  prev_refresh_hash  text,                   -- the one before: seeing it again = stolen token -> revoke
  created_at         timestamptz not null default now(),
  last_seen_at       timestamptz not null default now(),
  revoked_at         timestamptz
);
create index devices_user on public.devices (user_id);
create unique index devices_refresh on public.devices (refresh_token_hash);
create index devices_prev_refresh on public.devices (prev_refresh_hash);
alter table public.devices enable row level security;   -- no policies: server only

-- ---------------------------------------------------------------------------------------------
-- device_codes: "KQ7M-2TXP" shown in the plug-in, approved on /link. Server only.

create table public.device_codes (
  device_code_hash text primary key,                          -- SHA-256 (hex) of the plug-in's secret code
  user_code        text not null unique check (user_code ~ '^[A-HJ-NP-Z2-9]{8}$'),   -- no 0 O 1 I
  client_info      jsonb not null default '{}'::jsonb,         -- plug-in, version, OS, DAW, computer: never tracks or audio
  status           text not null default 'pending' check (status in ('pending', 'approved', 'denied', 'used')),
  user_id          uuid references auth.users (id) on delete cascade,
  created_at       timestamptz not null default now(),
  expires_at       timestamptz not null default now() + interval '10 minutes',
  last_poll_at     timestamptz
);
create index device_codes_expiry on public.device_codes (expires_at);
alter table public.device_codes enable row level security;   -- no policies: server only

-- ---------------------------------------------------------------------------------------------
-- settings_sync: only Appearance + stem names follow the account (prompt 5C item 6). Audio
-- settings (peak ceiling, sync safety, line-up limit, bus name) never sync.

create table public.settings_sync (
  user_id    uuid primary key references auth.users (id) on delete cascade,
  appearance jsonb not null default '{}'::jsonb,   -- language, theme, uiScale, glassAlpha, hostColours, reduceMotion
  stem_names jsonb not null default '[]'::jsonb,
  updated_at timestamptz not null default now()
);
create trigger settings_touch before update on public.settings_sync for each row execute function public.touch_updated_at();
alter table public.settings_sync enable row level security;
create policy "settings: owner reads"   on public.settings_sync for select using (user_id = auth.uid());
create policy "settings: owner writes"  on public.settings_sync for insert with check (user_id = auth.uid());
create policy "settings: owner updates" on public.settings_sync for update using (user_id = auth.uid()) with check (user_id = auth.uid());

-- ---------------------------------------------------------------------------------------------
-- share_links: permanent links registered by a signed-in Hub (A6 item 2). Server only.

create table public.share_links (
  id         uuid primary key default gen_random_uuid(),
  user_id    uuid not null references auth.users (id) on delete cascade,
  token_hash text not null unique,              -- SHA-256 of the link token; the token itself lives in the Hub
  kind       text not null check (kind in ('listen', 'send')),
  label      text not null default '' check (char_length(label) <= 64),
  created_at timestamptz not null default now(),
  revoked_at timestamptz
);
create index share_links_user on public.share_links (user_id);
alter table public.share_links enable row level security;   -- no policies: server only

-- ---------------------------------------------------------------------------------------------
-- saved_friends: names to invite again; never the send-in link tokens

create table public.saved_friends (
  id             uuid primary key default gen_random_uuid(),
  owner_id       uuid not null references auth.users (id) on delete cascade,
  name           text not null check (char_length(name) between 1 and 40),
  friend_user_id uuid references auth.users (id) on delete set null,
  created_at     timestamptz not null default now()
);
create index saved_friends_owner on public.saved_friends (owner_id);
alter table public.saved_friends enable row level security;
create policy "friends: owner reads"   on public.saved_friends for select using (owner_id = auth.uid());
create policy "friends: owner inserts" on public.saved_friends for insert with check (owner_id = auth.uid());
create policy "friends: owner deletes" on public.saved_friends for delete using (owner_id = auth.uid());

-- ---------------------------------------------------------------------------------------------
-- storage: profile photos, public read, written by the API only (resized to 512 x 512 WebP)

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('avatars', 'avatars', true, 1048576, array['image/webp'])
on conflict (id) do nothing;
-- no insert / update / delete policies on storage.objects for "avatars": only the secret key writes

-- ---------------------------------------------------------------------------------------------
-- housekeeping (pg_cron, if enabled): expired codes, accounts deleted more than 30 days ago
create or replace function public.purge_expired() returns void
language plpgsql security definer set search_path = public as $$
begin
  delete from public.device_codes where expires_at < now() - interval '1 day';
  delete from auth.users u using public.profiles p
   where p.id = u.id and p.deleted_at is not null and p.deleted_at < now() - interval '30 days';
end $$;

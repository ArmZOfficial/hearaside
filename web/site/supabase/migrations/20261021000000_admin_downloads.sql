-- Admin back office: download counter (per day, per version). Server only (secret key).
-- NOT applied to production until the owner approves it. Apply with:
--   supabase db push            (linked project)   or   psql "$DATABASE_URL" -f this file
--
-- source = 'site'   counted live by GET /api/v1/download/<version> (one row per day)
--          'github' number imported from GitHub Releases (replaced on every import)
--          'manual' a number the admin typed in for downloads made before counting started
-- github / manual rows use the fixed day 2000-01-01 ("before counting"), so each is one row per version.

create table if not exists public.download_daily (
  day     date    not null,
  version text    not null check (version ~ '^[0-9]+\.[0-9]+\.[0-9]+$'),
  source  text    not null check (source in ('site', 'github', 'manual')),
  count   integer not null default 0 check (count >= 0),
  primary key (day, version, source)
);
alter table public.download_daily enable row level security;   -- no policies: server only

-- one atomic +1 (a read-then-write from the API would lose counts under concurrent downloads)
create or replace function public.bump_download(p_version text) returns void
language sql security definer set search_path = public as $$
  insert into public.download_daily (day, version, source, count)
  values ((now() at time zone 'utc')::date, p_version, 'site', 1)
  on conflict (day, version, source) do update set count = public.download_daily.count + 1
$$;
revoke execute on function public.bump_download(text) from public, anon, authenticated;
grant execute on function public.bump_download(text) to service_role;

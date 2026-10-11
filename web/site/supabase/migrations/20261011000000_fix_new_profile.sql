-- Every sign-up failed with "Database error saving new user": new_profile() passed a text handle to
-- handle_reserved(citext), and Postgres has no implicit text -> citext cast for function calls.
create or replace function public.new_profile() returns trigger
language plpgsql security definer set search_path = public as $$
declare
  base text := left(regexp_replace(lower(split_part(coalesce(new.email, 'user'), '@', 1)), '[^a-z0-9_]', '', 'g'), 14);
  candidate citext;
  tries int := 0;
begin
  if char_length(base) < 3 then base := 'user'; end if;
  loop
    candidate := (base || lpad((floor(random() * 10000))::int::text, 4, '0'))::citext;
    exit when not exists (select 1 from public.profiles p where p.handle = candidate) and not public.handle_reserved(candidate);
    tries := tries + 1;
    if tries > 20 then candidate := ('user' || substr(md5(new.id::text), 1, 12))::citext; exit; end if;
  end loop;
  insert into public.profiles (id, handle, display_name, locale)
  values (new.id, candidate,
          left(coalesce(new.raw_user_meta_data ->> 'display_name', new.raw_user_meta_data ->> 'full_name', new.raw_user_meta_data ->> 'name', ''), 40),
          case when new.raw_user_meta_data ->> 'locale' = 'th' then 'th' else 'en' end);
  return new;
end $$;

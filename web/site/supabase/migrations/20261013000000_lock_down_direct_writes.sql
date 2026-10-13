-- Security review 2026-10-13. The website API writes with the secret key, so signed-in users never need
-- to write these tables themselves. Without these policies a user could call PostgREST directly with the
-- publishable key and their own session and skip the API's checks (handle rate limit, settings schema,
-- the 50 saved-friends cap, a free-form avatar path).

drop policy if exists "profiles: owner updates" on public.profiles;
drop policy if exists "settings: owner writes"  on public.settings_sync;
drop policy if exists "settings: owner updates" on public.settings_sync;
drop policy if exists "friends: owner inserts"  on public.saved_friends;
drop policy if exists "friends: owner deletes"  on public.saved_friends;

-- purge_expired() is for pg_cron / the secret key only: PostgREST would otherwise expose it as an RPC.
revoke execute on function public.purge_expired() from public, anon, authenticated;

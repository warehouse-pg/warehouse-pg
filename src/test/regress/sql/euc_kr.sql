-- This test is about EUC_KR encoding, chosen as perhaps the most prevalent
-- non-UTF8, multibyte encoding as of 2026-01.  Since UTF8 can represent all
-- of EUC_KR, also run the test in UTF8.
--
-- Upstream skips the test in any other encoding with psql's \if, which
-- arrived in PG10; this branch's psql has no way to skip the rest of a
-- script, so the test simply runs.  gpinitsystem (and hence gpdemo) creates
-- the cluster with UTF-8, so the guard would never fire here anyway.

-- Exercise is_multibyte_char_in_char (non-UTF8) slow path.
SELECT POSITION(
	convert_from('\xbcf6c7d0', 'EUC_KR') IN
	convert_from('\xb0fac7d02c20bcf6c7d02c20b1e2bcfa2c20bbee', 'EUC_KR'));

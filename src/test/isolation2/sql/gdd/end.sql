-- start_ignore
!\retcode gpconfig -c gp_enable_global_deadlock_detector -v off;
!\retcode gpconfig -c gp_global_deadlock_detector_period -v 120;
!\retcode gpconfig -c autovacuum -v on;
!\retcode gpstop -rai;
-- end_ignore

-- Start new session on coordinator to make sure it has fully completed
-- recovery and up and running again.
1: SHOW gp_enable_global_deadlock_detector;
1: SHOW gp_global_deadlock_detector_period;

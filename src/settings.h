#define TIMEOUT_US 1000000
#define RESET_US 300000

// Settings used to auto-tune the glitch length
#define TUNE_DURATION_MAX 800   // Something long enough to crash the target
#define TUNE_INITIAL_STEP (TUNE_DURATION_MAX / 10)
#define TUNE_TRIGGERS_PER_SECOND 6  // Expected trigger count 1s after boot

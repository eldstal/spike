#include <zephyr.h>
#include <device.h>
#include <drivers/counter.h>
#include <drivers/gpio.h>
#include <sys/printk.h>

#include "settings.h"

/*
 * Current application state
 */

enum GlitchState {
  IDLE = 0,
  START,
  TUNE_GLITCH,
  WAIT_TRIGGER,
  WAIT_GLITCH,
  GLITCHING,
};

struct {
  uint32_t offset_ns = 4000000;
  uint32_t duration_us = 1000;    // Probably too long.
                                  // This is used as a starting point for tuning.

  // State while tuning parameters
  struct {
    uint32_t success = 0;
  } tune;

  GlitchState state = IDLE;
} GLITCH;

const struct device *timer_device;

/*
 * Interface to target device
 */

// GPIO mappings from the devicetree
// If you want to use other pins, change peripherals.overlay
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)
const struct gpio_dt_spec pin_trigger = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, target_trigger_gpios);
const struct gpio_dt_spec pin_power = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, target_power_gpios);
const struct gpio_dt_spec pin_success = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, target_success_gpios);


/*
 * Timeout on the trigger signal
 * This will force a hard reset of the target device
 */

void set_target_power(bool on) {
  gpio_pin_set_dt(&pin_power, on);
}

void cut_target_power(uint32_t us) {
  set_target_power(false);
  k_busy_wait(us);
  set_target_power(true);
};

struct k_timer timeout_timer;

void timeout_fun(struct k_timer* timer) {

  if (GLITCH.state != WAIT_TRIGGER) return;

  printk("Target crashed. Resetting. Glitch delay: %u ns. Glitch length: %u us.\n",
         GLITCH.offset_ns, GLITCH.duration_us);

  cut_target_power(RESET_US);

}



/*
 * Timing of the glitch signal
 */

struct k_timer glitch_timer;

void glitch_fun(struct k_timer* timer) {

  if (GLITCH.state != WAIT_GLITCH) return;

  GLITCH.state = GLITCHING;
  cut_target_power(GLITCH.duration_us);

  //printk("Sent glitch.\n");

  GLITCH.state = WAIT_TRIGGER;

  // Set up the timeout, in case we crash the target
  k_timer_start(&timeout_timer, K_USEC(TIMEOUT_US), K_NO_WAIT);

}

/*
 * Trigger from the target device
 * Start counting down to the glitch
 */

static struct gpio_callback trigger_cb_data;

void trigger_isv(const struct device* dev,
                      struct gpio_callback *cb,
                      uint32_t pins) {
  if (GLITCH.state == START) {
    printk("Initial trigger signal received. Starting glitch delay.\n");
    GLITCH.state = WAIT_TRIGGER;
  }

  if (GLITCH.state == TUNE_GLITCH) {
    GLITCH.tune.success += 1;
    return;
  }

  if (GLITCH.state != WAIT_TRIGGER) return;

  // Looks like the target didn't crash after all. Nice.
  k_timer_stop(&timeout_timer);

  // TODO: Detect offset longer than trigger period
  GLITCH.state = WAIT_GLITCH;
  GLITCH.offset_ns += 100;
  k_timer_start(&glitch_timer, K_NSEC(GLITCH.offset_ns), K_NO_WAIT);

}


void stop_all() {
  GLITCH.state = IDLE;
  k_timer_stop(&timeout_timer);
  k_timer_stop(&glitch_timer);
}

/*
 * Success signal from the target device
 * Stop all the glitching and report the success
 */
static struct gpio_callback success_cb_data;

void success_isv(const struct device* dev,
                      struct gpio_callback *cb,
                      uint32_t pins) {
  printk("Success signal received. Glitch delay: %u ns. Glitch length: %u us.\n",
         GLITCH.offset_ns, GLITCH.duration_us);

  stop_all();

}


bool setup_interrupts() {
  // Pin change interrupt for the trigger signal
  // As soon as that pin has a leading edge, we start the countdown to glitch.
  // Using ACTIVE rather than HIGH here means we respect the ACTIVE_HIGH setting in the
  // device tree. Feel free to flip that to ACTIVE_LOW if your trigger signal is that way.
  int ret = gpio_pin_interrupt_configure_dt(&pin_trigger, GPIO_INT_EDGE_TO_ACTIVE);
  if (ret != 0) {
    printk("Error %d: Unable to configure pin interrupt for trigger signal.\n", ret);
    return false;
  }

  gpio_init_callback(&trigger_cb_data, trigger_isv, BIT(pin_trigger.pin));

  ret = gpio_add_callback(pin_trigger.port, &trigger_cb_data);
  if (ret != 0) {
    printk("Error %d: Unable to register callback for trigger signal.\n", ret);
    return false;
  }


  // Pin change interrupt for the success signal
  ret = gpio_pin_interrupt_configure_dt(&pin_success, GPIO_INT_EDGE_TO_ACTIVE);
  if (ret != 0) {
    printk("Error %d: Unable to configure pin interrupt for success signal.\n", ret);
    return false;
  }

  gpio_init_callback(&success_cb_data, success_isv, BIT(pin_success.pin));

  ret = gpio_add_callback(pin_success.port, &success_cb_data);
  if (ret != 0) {
    printk("Error %d: Unable to register callback for success signal.\n", ret);
    return false;
  }

  return true;

}

bool setup_timer() {

  return true;
}

uint32_t tune_glitch_length() {
  // Try different glitch lengths and find the longest one that
  // reliably doesn't crash the target device.

  // Start at something very long, it should crash the device.
  uint32_t duration = TUNE_DURATION_MAX;
  int32_t step = -(TUNE_INITIAL_STEP);

  GLITCH.state = TUNE_GLITCH;

  uint32_t best_count = 1;
  uint32_t best_duration = 0;

  set_target_power(true);

  // Keep shortening until the device stops crashing
  while (duration > 0) {

    GLITCH.tune.success = 0;

    for (uint32_t n=0; n<1000; n++) {
      cut_target_power(duration);
      k_busy_wait(1000);
    }

    uint32_t count = GLITCH.tune.success;

    if (count >= best_count) {
      // The device didn't super crash
      best_count = GLITCH.tune.success;
      best_duration = duration;

      // Start stepping upward
      if (count >= TUNE_TRIGGERS_PER_SECOND-1) {
        step = duration / 20;
        printk("Survived glitch duration %u. Tip-toeing upward.\n", duration);
      }
    } else {
      if (step > 0) {
        // We've started increasing the duration and the device crashed.
        // That means the previous best duration is our bound.
        break;
      }
    }

    //printk("Tried glitch duration of %u us, survived %u trigger cycles.\n", 
    //       duration, GLITCH.tune.success);

    duration += step;
  }

  set_target_power(false);

  printk("Settled on glitch duration of %u us, which passed %u trigger cycles.\n", 
         best_duration, best_count);
  GLITCH.state = IDLE;

  return best_duration;
}


int main() {

  printk("Booting up SPIKE\n");

  k_timer_init(&timeout_timer, timeout_fun, NULL);
  k_timer_init(&glitch_timer, glitch_fun, NULL);

  gpio_pin_configure_dt(&pin_trigger, GPIO_INPUT);
  gpio_pin_configure_dt(&pin_power, GPIO_OUTPUT_LOW);
  gpio_pin_configure_dt(&pin_success, GPIO_INPUT);

  if (!setup_interrupts()) return 1;


  printk("Tuning glitch duration...\n");
  GLITCH.duration_us = tune_glitch_length();

  GLITCH.state = START;

  printk("Starting up target...\n");
  cut_target_power(RESET_US);
  printk("Target started. Waiting for trigger.\n");


  while (true) { k_sleep(K_FOREVER); }

  return 0;
}




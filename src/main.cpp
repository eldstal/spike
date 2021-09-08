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
  BOOT = 0,
  WAIT_TRIGGER,
  WAIT_GLITCH,
  GLITCHING,
  IDLE,
};

struct Glitch {
  uint32_t offset_us = 0;
  uint32_t duration_us = 1050;
  GlitchState state = BOOT;
};
Glitch GLITCH;

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

void cut_target_power(uint32_t us) {
  gpio_pin_set_dt(&pin_power, 0);
  k_busy_wait(us);
  gpio_pin_set_dt(&pin_power, 1);
};

struct k_timer timeout_timer;

void timeout_fun(struct k_timer* timer) {

  if (GLITCH.state != WAIT_TRIGGER) return;

  printk("Target crashed. Resetting. Glitch delay: %u us. Glitch length: %u us.\n",
         GLITCH.offset_us, GLITCH.duration_us);

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
  if (GLITCH.state == BOOT) {
    printk("Initial trigger signal received. Starting glitch delay.\n");
    GLITCH.state = WAIT_TRIGGER;
  }

  if (GLITCH.state != WAIT_TRIGGER) return;

  // Looks like the target didn't crash after all. Nice.
  k_timer_stop(&timeout_timer);

  // TODO: Detect offset longer than trigger period
  GLITCH.state = WAIT_GLITCH;
  GLITCH.offset_us += 1;
  k_timer_start(&glitch_timer, K_USEC(GLITCH.offset_us), K_NO_WAIT);

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
  printk("Success signal received. Glitch delay: %u us. Glitch length: %u us.\n",
         GLITCH.offset_us, GLITCH.duration_us);

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


int main() {

  printk("Booting up SPIKE\n");

  k_timer_init(&timeout_timer, timeout_fun, NULL);
  k_timer_init(&glitch_timer, glitch_fun, NULL);

  gpio_pin_configure_dt(&pin_trigger, GPIO_INPUT);
  gpio_pin_configure_dt(&pin_power, GPIO_OUTPUT_LOW);
  gpio_pin_configure_dt(&pin_success, GPIO_INPUT);

  if (!setup_interrupts()) return 1;

  GLITCH.state = BOOT;

  printk("Starting up target\n");
  cut_target_power(RESET_US);
  printk("Target started. Waiting for trigger.\n");


  while (true) { k_sleep(K_FOREVER); }

  return 0;
}




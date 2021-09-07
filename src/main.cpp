#include <zephyr.h>
#include <device.h>
#include <drivers/counter.h>
#include <drivers/gpio.h>
#include <sys/printk.h>

#include "settings.h"


/*
 * Current glitching parameters
 */
struct Glitch {
  uint32_t offset_us = 0;
  uint32_t duration_us = 1;
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
struct counter_alarm_cfg timeout_alarm_cfg;
void timeout_isv(const struct device *counter_dev,
                 uint8_t chan_id, uint32_t ticks,
                 void *user_data) {

  printk("Target device timeout. Resetting.\n");
  gpio_pin_set_dt(&pin_power, 0);
  k_busy_wait(RESET_US);
  gpio_pin_set_dt(&pin_power, 1);
}



/*
 * Timing of the glitch signal
 */
struct counter_alarm_cfg glitch_alarm_cfg;
void glitch_isv(const struct device *counter_dev,
                 uint8_t chan_id, uint32_t ticks,
                 void *user_data) {

}

/*
 * Trigger from the target device
 * Start counting down to the glitch
 */

static struct gpio_callback trigger_cb_data;

void trigger_isv(const struct device* dev,
                      struct gpio_callback *cb,
                      uint32_t pins) {
  printk("Trigger signal received. Setting up glitch.\n");
  glitch_alarm_cfg.ticks = counter_us_to_ticks(timer_device, GLITCH.offset_us);

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
  timer_device = device_get_binding(DT_LABEL(DT_ALIAS(glitch_timer)));
  if (timer_device == NULL) {
    printk("Error intitializing timer.\n");
    return false;
  }

  // This timer is used for two separate alarms.
  // The glitch alarm is used to control the delay between ready and glitch
  // The timeout is used after the glitch to detect a crashed target system.

  glitch_alarm_cfg.flags = 0;
  glitch_alarm_cfg.ticks = counter_us_to_ticks(timer_device, TIMEOUT_US);
  glitch_alarm_cfg.callback = glitch_isv;
  glitch_alarm_cfg.user_data = NULL;

  timeout_alarm_cfg.flags = 0;
  timeout_alarm_cfg.ticks = counter_us_to_ticks(timer_device, TIMEOUT_US);
  timeout_alarm_cfg.callback = timeout_isv;
  timeout_alarm_cfg.user_data = NULL;

  return true;
}


int main() {

  printk("Booting up SPIKE\n");

  gpio_pin_configure_dt(&pin_trigger, GPIO_INPUT);
  gpio_pin_configure_dt(&pin_power, GPIO_OUTPUT_LOW);
  gpio_pin_configure_dt(&pin_success, GPIO_INPUT);

  if (!setup_interrupts()) return 1;
  if (!setup_timer()) return 2;


  while (true) { k_sleep(K_FOREVER); }

  return 0;
}




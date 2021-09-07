#include <drivers/gpio.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

// GPIO mappings from the devicetree
// If you want to use other pins, change pins.overlay
const struct gpio_dt_spec pin_trigger = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, target_trigger_gpios);
const struct gpio_dt_spec pin_power = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, target_power_gpios);
const struct gpio_dt_spec pin_success = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, target_success_gpios);

static struct gpio_callback trigger_cb_data;

void target_interrupt(const struct device* dev, struct gpio_callback *cb, uint32_t pins) {
  printk("Trigger signal received. Starting timer.\n");

}

int main() {

  printk("Booting up SPIKE\n");

  gpio_pin_configure_dt(&pin_trigger, GPIO_INPUT);
  gpio_pin_configure_dt(&pin_power, GPIO_OUTPUT_LOW);
  gpio_pin_configure_dt(&pin_success, GPIO_INPUT);

  // Here's how to use the pins directly
  int val = gpio_pin_get_dt(&pin_trigger);
  gpio_pin_set_dt(&pin_power, 0);

  // Pin change interrupt for the trigger signal
  // As soon as that pin has a leading edge, we start the countdown to glitch.
  // Using ACTIVE rather than HIGH here means we respect the ACTIVE_HIGH setting in the
  // device tree. Feel free to flip that to ACTIVE_LOW if your trigger signal is that way.
  int ret = gpio_pin_interrupt_configure_dt(&pin_trigger, GPIO_INT_EDGE_TO_ACTIVE);
  if (ret != 0) {
    printk("Error %d: Unable to configure pin interrupt for trigger signal.\n", ret);
  }

  gpio_init_callback(&trigger_cb_data, target_interrupt, BIT(pin_trigger.pin));
  gpio_add_callback(pin_trigger.port, &trigger_cb_data);

  while (true) { }

  return 1;
}

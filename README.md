# CANopenNode v4.x Protocol Stack for Zephyr RTOS

## What is this

[CANopenNode](https://github.com/CANopenNode/CANopenNode) is a free and open source CANopen
protocol stack written in C. This repo, CANopenNodeZephyr, is the glue layer that connects it
to the [Zephyr RTOS](https://github.com/zephyrproject-rtos/zephyr). It has the Zephyr CAN driver,
Kconfig options, CMake build files, and a working sample.

This code has been tested on STM32F072 (bxCAN), STM32H7(M_CAN), and FRDM-MCXN947 (FlexCAN) series. 

### Files

| Path | What it does |
|---|---|
| `CANopenNode/` | Upstream CANopenNode v4.x (git submodule) |
| `src/CO_driver.c` | Zephyr CAN driver glue: TX workqueue, RX filters, mutexes |
| `src/canopen_leds.c` | CiA 303-3 LED indicators over GPIO |
| `src/canopen_sync.c` | Separate SYNC producer/consumer thread |
| `include/CO_driver_target.h` | Zephyr-specific macros |
| `Kconfig` | All `CONFIG_CANOPENNODE_*` options |
| `zephyr/module.yml` | Zephyr module config |
| `samples/canopennode/` | Sample app with OD and LEDs |

License: **Apache-2.0**

### What is NOT included

The sample is kept simple on purpose. These features are left out so each project
can add them the way they need:

- **NVS storage** - saving the Object Dictionary to flash is not in the default
  sample. If you need it, add `CONFIG_CANOPENNODE_STORAGE=y`, enable the NVS and
  Settings subsystems, and make sure your board has a `storage` partition.
- **Firmware upgrade over CANopen** - not built in. This needs MCUboot, flash
  map, stream flash, and the `canopen` runner.

The sample ships with `native_sim.conf` as a reference config for simulation builds.

---


## How to add it to your Zephyr workspace

### Method 1: As a West project

Add this to your workspace `west.yml`:

```yaml
manifest:
  projects:
    - name: canopennodezephyr
      url: https://github.com/zephyrproject-rtos/CANopenNodeZephyr.git
      revision: main
      submodules:
        - path: CANopenNode
      path: custom/canopennodezephyr
```

Then:

```bash
west update
```

### Method 2: As a submanifest

Make `zephyr/submanifests/canopennodezephyr.yaml` with same content as above,
then `west update`.

## Build and flash

### Quick start for any board with CAN

```bash
west build -b <board> custom/canopennodezephyr/samples/canopennode
west flash
```

### NXP FRDM-MCXN947

Has onboard CAN (FlexCAN). Build and flash:

```bash
west build -p always -b frdm_mcxn947/mcxn947/cpu0 \
    custom/canopennodezephyr/samples/canopennode
west flash
```

User LEDs: `led0` = red, `led1` = green (CANopen status LEDs per CiA 303-3).


---

## Config options

All from `Kconfig`. Set in `prj.conf` or with `-DCONFIG_<NAME>=<value>` on the build line.

| Option | Type | Default | What it does |
|---|---|---|---|
| `CANOPENNODE` | bool | n | Enable CANopenNode |
| `CANOPENNODE_SDO_BUFFER_SIZE` | int | 32 | SDO buffer size (7-889 bytes) |
| `CANOPENNODE_TRACE_BUFFER_SIZE` | int | 100 | Trace buffer in bytes |
| `CANOPENNODE_TX_WORKQUEUE_STACK_SIZE` | int | 512 | TX workqueue thread stack |
| `CANOPENNODE_TX_WORKQUEUE_PRIORITY` | int | 0 / -1 | TX workqueue priority |
| `CANOPENNODE_STORAGE` | bool | y* | Store OD in NVS (needs `SETTINGS`, off in default sample) |
| `CANOPENNODE_STORAGE_HANDLER_ERASES_EEPROM` | bool | n | Erase EEPROM on write to 0x1011:01 |
| `CANOPENNODE_LEDS` | bool | y | CiA 303-3 LED indicators |
| `CANOPENNODE_LEDS_BICOLOR` | bool | n | Handle LEDs as one bicolor LED |
| `CANOPENNODE_SYNC_THREAD` | bool | n | Separate SYNC thread |
| `CANOPEN_NODE_ID` | int | 10 | CANopen Node ID (1-127) |

### Example: change node ID, turn off LEDs

```bash
west build -b frdm_mcxn947/mcxn947/cpu0 \
    custom/canopennodezephyr/samples/canopennode \
    -- -DCONFIG_CANOPEN_NODE_ID=42 -DCONFIG_CANOPENNODE_LEDS=n
```

## License

Apache-2.0, see [LICENSE](LICENSE).

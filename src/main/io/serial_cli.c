/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "platform.h"
#include "version.h"

#include "scheduler/scheduler.h"

#include "build_config.h"

#include "common/utils.h"
#include "common/axis.h"
#include "common/maths.h"
#include "common/color.h"
#include "common/typeconversion.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/system.h"

#include "drivers/sensor.h"
#include "drivers/accgyro.h"
#include "drivers/compass.h"

#include "drivers/serial.h"
#include "drivers/bus_i2c.h"
#include "drivers/gpio.h"
#include "drivers/timer.h"
#include "drivers/pwm_rx.h"

#include "drivers/buf_writer.h"

//#include "io/escservo.h"
#include "io/gps.h"
#include "io/gimbal.h"
#include "io/rc_controls.h"
#include "io/serial.h"
#include "io/ledstrip.h"
#include "io/flashfs.h"
#include "io/beeper.h"

#include "rx/rx.h"
#include "rx/spektrum.h"

#include "sensors/battery.h"
#include "sensors/boardalignment.h"
#include "sensors/sensors.h"
#include "sensors/acceleration.h"
#include "sensors/gyro.h"
#include "sensors/compass.h"
#include "sensors/barometer.h"

#include "blackbox/blackbox.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/navigation_rewrite.h"
#include "flight/failsafe.h"

#include "telemetry/telemetry.h"
#include "telemetry/frsky.h"
#include "telemetry/hott.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "config/config_profile.h"
#include "config/config_system.h"
#include "config/feature.h"
#include "config/profile.h"
#include "config/persistent_flags.h"

#include "common/printf.h"

#include "serial_cli.h"

// FIXME remove this for targets that don't need a CLI.  Perhaps use a no-op macro when USE_CLI is not enabled
// signal that we're in cli mode
uint8_t cliMode = 0;

#ifdef USE_CLI

extern uint16_t cycleTime; // FIXME dependency on mw.c

void gpsEnablePassthrough(serialPort_t *gpsPassthroughPort);

static serialPort_t *cliPort;
static bufWriter_t *cliWriter;
static uint8_t cliWriteBuffer[sizeof(*cliWriter) + 16];

static void cliAux(char *cmdline);
static void cliRxFail(char *cmdline);
static void cliAdjustmentRange(char *cmdline);
static void cliMotorMix(char *cmdline);
static void cliDefaults(char *cmdline);
static void cliDump(char *cmdLine);
static void cliExit(char *cmdline);
static void cliFeature(char *cmdline);
static void cliMotor(char *cmdline);
static void cliPlaySound(char *cmdline);
static void cliProfile(char *cmdline);
static void cliRateProfile(char *cmdline);
static void cliReboot(void);
static void cliSave(char *cmdline);
static void cliSerial(char *cmdline);

#ifdef USE_SERVOS
static void cliServo(char *cmdline);
static void cliServoMix(char *cmdline);
#endif

static void cliSet(char *cmdline);
static void cliGet(char *cmdline);
static void cliStatus(char *cmdline);
#ifndef SKIP_TASK_STATISTICS
static void cliTasks(char *cmdline);
#endif
static void cliVersion(char *cmdline);
static void cliRxRange(char *cmdline);
static void cliPFlags(char *cmdline);


#ifdef GPS
static void cliGpsPassthrough(char *cmdline);
#endif

static void cliHelp(char *cmdline);
static void cliMap(char *cmdline);

#ifdef LED_STRIP
static void cliLed(char *cmdline);
static void cliColor(char *cmdline);
#endif

#ifndef USE_QUAD_MIXER_ONLY
static void cliMixer(char *cmdline);
#endif

#ifdef USE_FLASHFS
static void cliFlashInfo(char *cmdline);
static void cliFlashErase(char *cmdline);
#ifdef USE_FLASH_TOOLS
static void cliFlashWrite(char *cmdline);
static void cliFlashRead(char *cmdline);
#endif
#endif

#ifdef BEEPER
static void cliBeeper(char *cmdline);
#endif

// buffer
static char cliBuffer[48];
static uint32_t bufferIndex = 0;

#ifndef USE_QUAD_MIXER_ONLY
//  this with mixerMode_e
static const char * const mixerNames[] = {
    "TRI", "QUADP", "QUADX", "BI",
    "GIMBAL", "Y6", "HEX6",
    "FLYING_WING", "Y4", "HEX6X", "OCTOX8", "OCTOFLATP", "OCTOFLATX",
    "AIRPLANE", "HELI_120_CCPM", "HELI_90_DEG", "VTAIL4",
    "HEX6H", "PPM_TO_SERVO", "DUALCOPTER", "SINGLECOPTER",
    "ATAIL4", "CUSTOM", "CUSTOMAIRPLANE", "CUSTOMTRI", NULL
};
#endif

// sync this with features_e
static const char * const featureNames[] = {
    "RX_PPM", "VBAT", "INFLIGHT_ACC_CAL", "RX_SERIAL", "MOTOR_STOP",
    "SERVO_TILT", "SOFTSERIAL", "GPS", "FAILSAFE",
    "SONAR", "TELEMETRY", "CURRENT_METER", "3D", "RX_PARALLEL_PWM",
    "RX_MSP", "RSSI_ADC", "LED_STRIP", "DISPLAY", "ONESHOT125",
    "BLACKBOX", "CHANNEL_FORWARDING", NULL
};

// sync this with rxFailsafeChannelMode_e
static const char rxFailsafeModeCharacters[] = "ahs";

static const rxFailsafeChannelMode_e rxFailsafeModesTable[RX_FAILSAFE_TYPE_COUNT][RX_FAILSAFE_MODE_COUNT] = {
    { RX_FAILSAFE_MODE_AUTO, RX_FAILSAFE_MODE_HOLD, RX_FAILSAFE_MODE_INVALID },
    { RX_FAILSAFE_MODE_INVALID, RX_FAILSAFE_MODE_HOLD, RX_FAILSAFE_MODE_SET }
};

#ifndef CJMCU
// sync this with sensors_e
static const char * const sensorTypeNames[] = {
    "GYRO", "ACC", "BARO", "MAG", "SONAR", "GPS", "GPS+MAG", NULL
};

#define SENSOR_NAMES_MASK (SENSOR_GYRO | SENSOR_ACC | SENSOR_BARO | SENSOR_MAG)

static const char * const sensorHardwareNames[4][11] = {
    { "", "None", "MPU6050", "L3G4200D", "MPU3050", "L3GD20", "MPU6000", "MPU6500", "FAKE", NULL },
    { "", "None", "ADXL345", "MPU6050", "MMA845x", "BMA280", "LSM303DLHC", "MPU6000", "MPU6500", "FAKE", NULL },
    { "", "None", "BMP085", "MS5611", "BMP280", "FAKE", NULL },
    { "", "None", "HMC5883", "AK8975", "FAKE", NULL }
};
#endif

typedef struct {
    const char *name;
#ifndef SKIP_CLI_COMMAND_HELP
    const char *description;
    const char *args;
#endif
    void (*func)(char *cmdline);
} clicmd_t;

#ifndef SKIP_CLI_COMMAND_HELP
#define CLI_COMMAND_DEF(name, description, args, method) \
{ \
    name , \
    description , \
    args , \
    method \
}
#else
#define CLI_COMMAND_DEF(name, description, args, method) \
{ \
    name, \
    method \
}
#endif

// should be sorted a..z for bsearch()
const clicmd_t cmdTable[] = {
    CLI_COMMAND_DEF("adjrange", "configure adjustment ranges", NULL, cliAdjustmentRange),
    CLI_COMMAND_DEF("aux", "configure modes", NULL, cliAux),
#ifdef LED_STRIP
    CLI_COMMAND_DEF("color", "configure colors", NULL, cliColor),
#endif
    CLI_COMMAND_DEF("defaults", "reset to defaults and reboot", NULL, cliDefaults),
    CLI_COMMAND_DEF("dump", "dump configuration",
        "[master|profile|rates]", cliDump),
    CLI_COMMAND_DEF("exit", NULL, NULL, cliExit),
    CLI_COMMAND_DEF("feature", "configure features",
        "list\r\n"
        "\t<+|->[name]", cliFeature),
#ifdef USE_FLASHFS
    CLI_COMMAND_DEF("flash_erase", "erase flash chip", NULL, cliFlashErase),
    CLI_COMMAND_DEF("flash_info", "show flash chip info", NULL, cliFlashInfo),
#ifdef USE_FLASH_TOOLS
    CLI_COMMAND_DEF("flash_read", NULL, "<length> <address>", cliFlashRead),
    CLI_COMMAND_DEF("flash_write", NULL, "<address> <message>", cliFlashWrite),
#endif
#endif
    CLI_COMMAND_DEF("get", "get variable value",
            "[name]", cliGet),
#ifdef GPS
    CLI_COMMAND_DEF("gpspassthrough", "passthrough gps to serial", NULL, cliGpsPassthrough),
#endif
    CLI_COMMAND_DEF("help", NULL, NULL, cliHelp),
#ifdef LED_STRIP
    CLI_COMMAND_DEF("led", "configure leds", NULL, cliLed),
#endif
    CLI_COMMAND_DEF("map", "configure rc channel order",
        "[<map>]", cliMap),
#ifndef USE_QUAD_MIXER_ONLY
    CLI_COMMAND_DEF("mixer", "configure mixer",
        "list\r\n"
        "\t<name>", cliMixer),
#endif
    CLI_COMMAND_DEF("mmix", "custom motor mixer", NULL, cliMotorMix),
    CLI_COMMAND_DEF("motor",  "get/set motor",
       "<index> [<value>]", cliMotor),
    CLI_COMMAND_DEF("play_sound", NULL,
        "[<index>]\r\n", cliPlaySound),
    CLI_COMMAND_DEF("profile", "change profile",
        "[<index>]", cliProfile),
    CLI_COMMAND_DEF("rateprofile", "change rate profile",
        "[<index>]", cliRateProfile),
    CLI_COMMAND_DEF("rxrange", "configure rx channel ranges", NULL, cliRxRange),
    CLI_COMMAND_DEF("rxfail", "show/set rx failsafe settings", NULL, cliRxFail),
    CLI_COMMAND_DEF("save", "save and reboot", NULL, cliSave),
    CLI_COMMAND_DEF("serial", "configure serial ports", NULL, cliSerial),
#ifdef USE_SERVOS
    CLI_COMMAND_DEF("servo", "configure servos", NULL, cliServo),
#endif
    CLI_COMMAND_DEF("set", "change setting",
        "[<name>=<value>]", cliSet),
    CLI_COMMAND_DEF("pflags", "get persistent flags", NULL, cliPFlags),
#ifdef USE_SERVOS
    CLI_COMMAND_DEF("smix", "servo mixer",
        "<rule> <servo> <source> <rate> <speed> <min> <max> <box>\r\n"
        "\treset\r\n"
        "\tload <mixer>\r\n"
        "\treverse <servo> <source> r|n", cliServoMix),
#endif
    CLI_COMMAND_DEF("status", "show status", NULL, cliStatus),
#ifndef SKIP_TASK_STATISTICS
    CLI_COMMAND_DEF("tasks", "show task stats", NULL, cliTasks),
#endif
    CLI_COMMAND_DEF("version", "show version", NULL, cliVersion),
#ifdef BEEPER
    CLI_COMMAND_DEF("beeper", "turn on/off beeper", "list\r\n"
            "\t<+|->[name]", cliBeeper),
#endif
};
#define CMD_COUNT (sizeof(cmdTable) / sizeof(clicmd_t))

static const char * const lookupTableOffOn[] = {
    "OFF", "ON"
};

static const char * const lookupTableUnit[] = {
    "IMPERIAL", "METRIC"
};

static const char * const lookupTableAlignment[] = {
    "DEFAULT",
    "CW0",
    "CW90",
    "CW180",
    "CW270",
    "CW0FLIP",
    "CW90FLIP",
    "CW180FLIP",
    "CW270FLIP"
};

#ifdef GPS
static const char * const lookupTableGPSProvider[] = {
    "NMEA", "UBLOX", "I2C-NAV", "NAZA"
};

static const char * const lookupTableGPSSBASMode[] = {
    "AUTO", "EGNOS", "WAAS", "MSAS", "GAGAN", "NONE"
};

static const char * const lookupTableGpsModel[] = {
    "PEDESTRIAN", "AIR_1G", "AIR_4G"
};
#endif

static const char * const lookupTableCurrentSensor[] = {
    "NONE", "ADC", "VIRTUAL"
};

static const char * const lookupTableGimbalMode[] = {
    "NORMAL", "MIXTILT"
};

static const char * const lookupTablePidController[] = {
    "UNUSED", "MWREWRITE", "LUX"
};

static const char * const lookupTableBlackboxDevice[] = {
    "SERIAL", "SPIFLASH"
};

static const char * const lookupTableSerialRX[] = {
    "SPEK1024",
    "SPEK2048",
    "SBUS",
    "SUMD",
    "SUMH",
    "XB-B",
    "XB-B-RJ01",
    "IBUS"
};

static const char * const lookupTableGyroLpf[] = {
    "256HZ",
    "188HZ",
    "98HZ",
    "42HZ",
    "20HZ",
    "10HZ"
};

static const char * const lookupTableFailsafeProcedure[] = {
    "SET-THR", "DROP", "RTH"
};

#ifdef NAV
static const char * const lookupTableNavControlMode[] = {
    "ATTI", "CRUISE"
};

static const char * const lookupTableNavRthAltMode[] = {
    "CURRENT", "EXTRA", "FIXED", "MAX", "AT_LEAST"
};
#endif

typedef struct lookupTableEntry_s {
    const char * const *values;
    const uint8_t valueCount;
} lookupTableEntry_t;

typedef enum {
    TABLE_OFF_ON = 0,
    TABLE_UNIT,
    TABLE_ALIGNMENT,
#ifdef GPS
    TABLE_GPS_PROVIDER,
    TABLE_GPS_SBAS_MODE,
    TABLE_GPS_DYN_MODEL,
#endif
#ifdef BLACKBOX
    TABLE_BLACKBOX_DEVICE,
#endif
    TABLE_CURRENT_SENSOR,
    TABLE_GIMBAL_MODE,
    TABLE_PID_CONTROLLER,
    TABLE_SERIAL_RX,
    TABLE_GYRO_LPF,
    TABLE_FAILSAFE_PROCEDURE,
#ifdef NAV
    TABLE_NAV_USER_CTL_MODE,
    TABLE_NAV_RTH_ALT_MODE,
#endif
} lookupTableIndex_e;

static const lookupTableEntry_t lookupTables[] = {
    { lookupTableOffOn, sizeof(lookupTableOffOn) / sizeof(char *) },
    { lookupTableUnit, sizeof(lookupTableUnit) / sizeof(char *) },
    { lookupTableAlignment, sizeof(lookupTableAlignment) / sizeof(char *) },
#ifdef GPS
    { lookupTableGPSProvider, sizeof(lookupTableGPSProvider) / sizeof(char *) },
    { lookupTableGPSSBASMode, sizeof(lookupTableGPSSBASMode) / sizeof(char *) },
    { lookupTableGpsModel, sizeof(lookupTableGpsModel) / sizeof(char *) },
#endif
#ifdef BLACKBOX
    { lookupTableBlackboxDevice, sizeof(lookupTableBlackboxDevice) / sizeof(char *) },
#endif
    { lookupTableCurrentSensor, sizeof(lookupTableCurrentSensor) / sizeof(char *) },
    { lookupTableGimbalMode, sizeof(lookupTableGimbalMode) / sizeof(char *) },
    { lookupTablePidController, sizeof(lookupTablePidController) / sizeof(char *) },
    { lookupTableSerialRX, sizeof(lookupTableSerialRX) / sizeof(char *) },
    { lookupTableGyroLpf, sizeof(lookupTableGyroLpf) / sizeof(char *) },
    { lookupTableFailsafeProcedure, sizeof(lookupTableFailsafeProcedure) / sizeof(char *) },
#ifdef NAV
    { lookupTableNavControlMode, sizeof(lookupTableNavControlMode) / sizeof(char *) },
    { lookupTableNavRthAltMode, sizeof(lookupTableNavRthAltMode) / sizeof(char *) },
#endif
};

#define VALUE_TYPE_OFFSET 0
#define VALUE_SECTION_OFFSET 4
#define VALUE_MODE_OFFSET 6

typedef enum {
    // value type
    VAR_UINT8 = (0 << VALUE_TYPE_OFFSET),
    VAR_INT8 = (1 << VALUE_TYPE_OFFSET),
    VAR_UINT16 = (2 << VALUE_TYPE_OFFSET),
    VAR_INT16 = (3 << VALUE_TYPE_OFFSET),
    VAR_UINT32 = (4 << VALUE_TYPE_OFFSET),
    VAR_FLOAT = (5 << VALUE_TYPE_OFFSET),

    // value section
    MASTER_VALUE = (0 << VALUE_SECTION_OFFSET),
    PROFILE_VALUE = (1 << VALUE_SECTION_OFFSET),
    CONTROL_RATE_VALUE = (2 << VALUE_SECTION_OFFSET),
    MIGRATED_MASTER_VALUE = (3 << VALUE_SECTION_OFFSET),

    // value mode
    MODE_DIRECT = (0 << VALUE_MODE_OFFSET),
    MODE_LOOKUP = (1 << VALUE_MODE_OFFSET)
} cliValueFlag_e;

#define VALUE_TYPE_MASK (0x0F)
#define VALUE_SECTION_MASK (0x30)
#define VALUE_MODE_MASK (0xC0)

typedef struct cliMinMaxConfig_s {
    const int32_t min;
    const int32_t max;
} cliMinMaxConfig_t;

typedef struct cliLookupTableConfig_s {
    const lookupTableIndex_e tableIndex;
} cliLookupTableConfig_t;

typedef union {
    cliLookupTableConfig_t lookup;
    cliMinMaxConfig_t minmax;

} cliValueConfig_t;

typedef struct {
    const char *name;
    const uint8_t type; // see cliValueFlag_e
    const cliValueConfig_t config;
    pgn_t pgn;
    uint16_t offset;
    persistent_flags_e pflags;
} __attribute__((packed)) clivalue_t;

const clivalue_t valueTable[] = {
    { "looptime",                   VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = {0, 9000} , PG_IMU_CONFIG, offsetof(imuConfig_t, looptime)},
    { "emf_avoidance",              VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_SYSTEM_CONFIG, offsetof(systemConfig_t, emf_avoidance)},
    { "i2c_highspeed",              VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_SYSTEM_CONFIG, offsetof(systemConfig_t, i2c_highspeed)},
    { "gyro_sync",                  VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_IMU_CONFIG, offsetof(imuConfig_t, gyroSync)},
    { "gyro_sync_denom",            VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 1,  32 } , PG_IMU_CONFIG, offsetof(imuConfig_t, gyroSyncDenominator)},

    { "mid_rc",                     VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 1200,  1700 } , PG_RX_CONFIG, offsetof(rxConfig_t, midrc)},
    { "min_check",                  VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_RX_CONFIG, offsetof(rxConfig_t, mincheck)},
    { "max_check",                  VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_RX_CONFIG, offsetof(rxConfig_t, maxcheck)},
    { "rssi_channel",               VAR_INT8   | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  MAX_SUPPORTED_RC_CHANNEL_COUNT } , PG_RX_CONFIG, offsetof(rxConfig_t, rssi_channel)},
    { "rssi_scale",                 VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { RSSI_SCALE_MIN,  RSSI_SCALE_MAX } , PG_RX_CONFIG, offsetof(rxConfig_t, rssi_scale)},
    { "rssi_ppm_invert",            VAR_INT8   | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_RX_CONFIG, offsetof(rxConfig_t, rssi_ppm_invert)},
    { "rc_smoothing",               VAR_INT8   | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_RX_CONFIG, offsetof(rxConfig_t, rcSmoothing)},

    { "rx_min_usec",                VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_PULSE_MIN,  PWM_PULSE_MAX } , PG_RX_CONFIG, offsetof(rxConfig_t, rx_min_usec)},
    { "rx_max_usec",                VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_PULSE_MIN,  PWM_PULSE_MAX } , PG_RX_CONFIG, offsetof(rxConfig_t, rx_max_usec)},

    { "serialrx_provider",          VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_SERIAL_RX } , PG_RX_CONFIG, offsetof(rxConfig_t, serialrx_provider)},
    { "sbus_inversion",             VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_RX_CONFIG, offsetof(rxConfig_t, sbus_inversion)},
    { "spektrum_sat_bind",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { SPEKTRUM_SAT_BIND_DISABLED,  SPEKTRUM_SAT_BIND_MAX} , PG_RX_CONFIG, offsetof(rxConfig_t, spektrum_sat_bind)},

    { "input_filtering_mode",       VAR_INT8   | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_DRIVER_PWM_RX_CONFIG, offsetof(pwmRxConfig_t, inputFilteringMode)},

    { "min_throttle",               VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_ESC_AND_SERVO_CONFIG, offsetof(escAndServoConfig_t, minthrottle)},
    { "max_throttle",               VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_ESC_AND_SERVO_CONFIG, offsetof(escAndServoConfig_t, maxthrottle)},
    { "min_command",                VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_ESC_AND_SERVO_CONFIG, offsetof(escAndServoConfig_t, mincommand)},
    { "servo_center_pulse",         VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_ESC_AND_SERVO_CONFIG, offsetof(escAndServoConfig_t, servoCenterPulse)},
    { "motor_pwm_rate",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 50,  32000 } , PG_ESC_AND_SERVO_CONFIG, offsetof(escAndServoConfig_t, motor_pwm_rate)},
    { "servo_pwm_rate",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 50,  498 } , PG_ESC_AND_SERVO_CONFIG, offsetof(escAndServoConfig_t, servo_pwm_rate)},


    { "3d_deadband_low",            VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_MOTOR_3D_CONFIG, offsetof(motor3DConfig_t, deadband3d_low)}, // FIXME upper limit should match code in the mixer, 1500 currently
    { "3d_deadband_high",           VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_MOTOR_3D_CONFIG, offsetof(motor3DConfig_t, deadband3d_high)}, // FIXME lower limit should match code in the mixer, 1500 currently,
    { "3d_neutral",                 VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX } , PG_MOTOR_3D_CONFIG, offsetof(motor3DConfig_t, neutral3d)},

    { "disarm_kill_switch",         VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_ARMING_CONFIG, offsetof(armingConfig_t, disarm_kill_switch)},
    { "auto_disarm_delay",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  60 } , PG_ARMING_CONFIG, offsetof(armingConfig_t, auto_disarm_delay)},
    { "max_arm_angle",              VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  180 } , PG_ARMING_CONFIG, offsetof(armingConfig_t, max_arm_angle)},

    { "small_angle",                VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  180 } , PG_IMU_CONFIG, offsetof(imuConfig_t, small_angle)},

    { "reboot_character",           VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 48,  126 } , PG_SERIAL_CONFIG, offsetof(serialConfig_t, reboot_character)},

#ifdef GPS
    { "gps_provider",               VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_GPS_PROVIDER }, PG_GPS_CONFIG, offsetof(gpsConfig_t, provider)},
    { "gps_sbas_mode",              VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_GPS_SBAS_MODE }, PG_GPS_CONFIG, offsetof(gpsConfig_t, sbasMode)},
    { "gps_dyn_model",              VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_GPS_DYN_MODEL }, PG_GPS_CONFIG, offsetof(gpsConfig_t, dynModel) },
    { "gps_auto_config",            VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_GPS_CONFIG, offsetof(gpsConfig_t, autoConfig)},
    { "gps_auto_baud",              VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_GPS_CONFIG, offsetof(gpsConfig_t, autoBaud)},
#endif

#ifdef NAV
    { "nav_alt_p",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, P8[PIDALT]) },
    { "nav_alt_i",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, I8[PIDALT]) },
    { "nav_alt_d",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, D8[PIDALT]) },

    { "nav_vel_p",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, P8[PIDVEL]) },
    { "nav_vel_i",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, I8[PIDVEL]) },
    { "nav_vel_d",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, D8[PIDVEL]) },

    { "nav_pos_p",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, P8[PIDPOS]) },
    { "nav_pos_i",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, I8[PIDPOS]) },
    { "nav_pos_d",                  VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, D8[PIDPOS]) },

    { "nav_posr_p",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, P8[PIDPOSR]) },
    { "nav_posr_i",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, I8[PIDPOSR]) },
    { "nav_posr_d",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, D8[PIDPOSR]) },

    { "nav_navr_p",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, P8[PIDNAVR]) },
    { "nav_navr_i",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, I8[PIDNAVR]) },
    { "nav_navr_d",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  255 }, PG_PID_PROFILE, offsetof(pidProfile_t, D8[PIDNAVR]) },
#endif

#if defined(NAV_AUTO_MAG_DECLINATION)
    { "inav_auto_mag_decl",         VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.automatic_mag_declination)},
#endif

    { "inav_accz_unarmedcal",       VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.accz_unarmed_cal)},
    { "inav_use_gps_velned",        VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.use_gps_velned)},
    { "inav_gps_delay",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  500 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.gps_delay_ms)},
    { "inav_gps_min_sats",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 5,  10}, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.gps_min_sats)},

    { "inav_w_z_baro_p",            VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_z_baro_p)},
    { "inav_w_z_gps_p",             VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_z_gps_p)},
    { "inav_w_z_gps_v",             VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_z_gps_v)},
    { "inav_w_xy_gps_p",            VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_xy_gps_p)},
    { "inav_w_xy_gps_v",            VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_xy_gps_v)},
    { "inav_w_z_res_v",             VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_z_res_v)},
    { "inav_w_xy_res_v",            VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_xy_res_v)},
    { "inav_w_acc_bias",            VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  1 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, inav.w_acc_bias)},

    { "nav_disarm_on_landing",      VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, flags.disarm_on_landing)},
    { "nav_use_midthr_for_althold", VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, flags.use_thr_mid_for_althold)},
    { "nav_extra_arming_safety",    VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, flags.extra_arming_safety)},
    { "nav_user_control_mode",      VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_NAV_USER_CTL_MODE }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, flags.user_control_mode)},

    { "nav_position_timeout",       VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, pos_failure_timeout)},
    { "nav_wp_radius",              VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  10000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, waypoint_radius)},
    { "nav_max_speed",              VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, max_speed)},
    { "nav_manual_speed",           VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, max_manual_speed)},
    { "nav_manual_climb_rate",      VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, max_manual_climb_rate)},
    { "nav_landing_speed",          VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 100,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, land_descent_rate)},
    { "nav_land_slowdown_minalt",   VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 50,  1000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, land_slowdown_minalt)},
    { "nav_land_slowdown_maxalt",   VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 500,  4000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, land_slowdown_maxalt)},
    { "nav_emerg_landing_speed",    VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 100,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, emerg_descent_rate)},

    { "nav_min_rth_distance",       VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  5000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, min_rth_distance)},
    { "nav_rth_tail_first",         VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, flags.rth_tail_first)},
    { "nav_rth_alt_mode",           VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_NAV_RTH_ALT_MODE }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, flags.rth_alt_control_style)},
    { "nav_rth_altitude",           VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 100,  65000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, rth_altitude)},

    { "nav_mc_bank_angle",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 15,  45 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, mc_max_bank_angle)},
    { "nav_mc_hover_thr",           VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 1000,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, mc_hover_throttle)},
    { "nav_mc_min_fly_thr",         VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 1000,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, mc_min_fly_throttle)},

    { "nav_fw_cruise_thr",          VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 1000,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_cruise_throttle)},
    { "nav_fw_min_thr",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 1000,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_min_throttle)},
    { "nav_fw_max_thr",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 1000,  2000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_max_throttle)},
    { "nav_fw_bank_angle",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 5,  45 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_max_bank_angle)},
    { "nav_fw_climb_angle",         VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 5,  45 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_max_climb_angle)},
    { "nav_fw_dive_angle",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 5,  45 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_max_dive_angle)},
    { "nav_fw_pitch2thr",           VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  100 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_pitch_to_throttle)},
    { "nav_fw_roll2pitch",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  200 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_roll_to_pitch)},
    { "nav_fw_loiter_radius",       VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  10000 }, PG_NAVIGATION_CONFIG, offsetof(navConfig_t, fw_loiter_radius)},

#ifdef TELEMETRY
    { "telemetry_switch",           VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_TELEMETRY_CONFIG, offsetof(telemetryConfig_t, telemetry_switch)},
    { "telemetry_inversion",        VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_TELEMETRY_CONFIG, offsetof(telemetryConfig_t, telemetry_inversion)},

    { "frsky_default_lattitude",    VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { -90.0,  90.0 } , PG_FRSKY_TELEMETRY_CONFIG, offsetof(frskyTelemetryConfig_t, gpsNoFixLatitude)},
    { "frsky_default_longitude",    VAR_FLOAT  | MIGRATED_MASTER_VALUE, .config.minmax = { -180.0,  180.0 } , PG_FRSKY_TELEMETRY_CONFIG, offsetof(frskyTelemetryConfig_t, gpsNoFixLongitude)},
    { "frsky_coordinates_format",   VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  FRSKY_FORMAT_NMEA } , PG_FRSKY_TELEMETRY_CONFIG, offsetof(frskyTelemetryConfig_t, frsky_coordinate_format)},
    { "frsky_unit",                 VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_UNIT } , PG_FRSKY_TELEMETRY_CONFIG, offsetof(frskyTelemetryConfig_t, frsky_unit)},
    { "frsky_vfas_precision",       VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { FRSKY_VFAS_PRECISION_LOW,  FRSKY_VFAS_PRECISION_HIGH } , PG_FRSKY_TELEMETRY_CONFIG, offsetof(frskyTelemetryConfig_t, frsky_vfas_precision)},

    { "hott_alarm_sound_interval",  VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  120 } , PG_HOTT_TELEMETRY_CONFIG, offsetof(hottTelemetryConfig_t, hottAlarmSoundInterval)},
#endif

    { "battery_capacity",           VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  20000 } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, batteryCapacity)},
    { "vbat_scale",                 VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { VBAT_SCALE_MIN,  VBAT_SCALE_MAX } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, vbatscale)},
    { "vbat_max_cell_voltage",      VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  50 } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, vbatmaxcellvoltage)},
    { "vbat_min_cell_voltage",      VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  50 } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, vbatmincellvoltage)},
    { "vbat_warning_cell_voltage",  VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  50 } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, vbatwarningcellvoltage)},
    { "current_meter_scale",        VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -10000,  10000 } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, currentMeterScale)},
    { "current_meter_offset",       VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  3300 } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, currentMeterOffset)},
    { "multiwii_current_meter_output", VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, multiwiiCurrentMeterOutput)},
    { "current_meter_type",         VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_CURRENT_SENSOR } , PG_BATTERY_CONFIG, offsetof(batteryConfig_t, currentMeterType)},

    { "align_gyro",                 VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_ALIGNMENT } , PG_SENSOR_ALIGNMENT_CONFIG, offsetof(sensorAlignmentConfig_t, gyro_align)},
    { "align_acc",                  VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_ALIGNMENT } , PG_SENSOR_ALIGNMENT_CONFIG, offsetof(sensorAlignmentConfig_t, acc_align)},
    { "align_mag",                  VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_ALIGNMENT } , PG_SENSOR_ALIGNMENT_CONFIG, offsetof(sensorAlignmentConfig_t, mag_align)},

    { "align_board_roll",           VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -1800,  3600 } , PG_BOARD_ALIGNMENT, offsetof(boardAlignment_t, rollDeciDegrees)},
    { "align_board_pitch",          VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -1800,  3600 } , PG_BOARD_ALIGNMENT, offsetof(boardAlignment_t, pitchDeciDegrees)},
    { "align_board_yaw",            VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -1800,  3600 } , PG_BOARD_ALIGNMENT, offsetof(boardAlignment_t, yawDeciDegrees)},

    { "gyro_lpf",                   VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_GYRO_LPF } , PG_GYRO_CONFIG, offsetof(gyroConfig_t, gyro_lpf)},
    { "moron_threshold",            VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  128 } , PG_GYRO_CONFIG, offsetof(gyroConfig_t, gyroMovementCalibrationThreshold)},

    { "imu_dcm_kp",                 VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  20000 }, PG_IMU_CONFIG, offsetof(imuConfig_t, dcm_kp_acc)},
    { "imu_dcm_ki",                 VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  20000 }, PG_IMU_CONFIG, offsetof(imuConfig_t, dcm_ki_acc)},
    { "imu_dcm_kp_mag",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  65535 }, PG_IMU_CONFIG, offsetof(imuConfig_t, dcm_kp_mag)},
    { "imu_dcm_ki_mag",             VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  65535 }, PG_IMU_CONFIG, offsetof(imuConfig_t, dcm_ki_mag)},

    { "deadband",                   VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  32 } , PG_PROFILE, offsetof(profile_t, rcControlsConfig.deadband)},
    { "yaw_deadband",               VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  100 } , PG_PROFILE, offsetof(profile_t, rcControlsConfig.yaw_deadband)},
    { "pos_hold_deadband",          VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 10,  250 } , PG_PROFILE, offsetof(profile_t, rcControlsConfig.pos_hold_deadband)},
    { "alt_hold_deadband",          VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 10,  250 } , PG_PROFILE, offsetof(profile_t, rcControlsConfig.alt_hold_deadband)},
    { "3d_deadband_throttle",       VAR_UINT16 | PROFILE_VALUE, .config.minmax = { PWM_RANGE_ZERO,  PWM_RANGE_MAX }, PG_RC_CONTROLS_CONFIG, offsetof(rcControlsConfig_t, deadband3d_throttle) },

    { "throttle_tilt_comp_str",     VAR_UINT8  | PROFILE_VALUE, .config.minmax = { 0,  100 } , PG_PROFILE, offsetof(profile_t, throttle_tilt_compensation_strength)},

    { "yaw_motor_direction",        VAR_INT8   | MIGRATED_MASTER_VALUE, .config.minmax = { -1,  1 } , PG_MIXER_CONFIG, offsetof(mixerConfig_t, yaw_motor_direction)},
    { "yaw_jump_prevention_limit",  VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { YAW_JUMP_PREVENTION_LIMIT_LOW,  YAW_JUMP_PREVENTION_LIMIT_HIGH } , PG_MIXER_CONFIG, offsetof(mixerConfig_t, yaw_jump_prevention_limit)},

#ifdef USE_SERVOS
    { "tri_unarmed_servo",          VAR_INT8   | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_MIXER_CONFIG, offsetof(mixerConfig_t, tri_unarmed_servo)},
    { "servo_lowpass_freq",         VAR_UINT16  | MIGRATED_MASTER_VALUE, .config.minmax = { 10,  400} , PG_MIXER_CONFIG, offsetof(mixerConfig_t, servo_lowpass_freq)},
    { "servo_lowpass_enable",       VAR_INT8   | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_MIXER_CONFIG, offsetof(mixerConfig_t, servo_lowpass_enable)},
#endif

    { "default_rate_profile",       VAR_UINT8  | PROFILE_VALUE , .config.minmax = { 0,  MAX_CONTROL_RATE_PROFILE_COUNT - 1 } , PG_PROFILE, offsetof(profile_t, defaultRateProfileIndex)},
    { "rc_rate",                    VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  250 } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, rcRate8)},
    { "rc_expo",                    VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  100 } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, rcExpo8)},
    { "rc_yaw_expo",                VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  100 } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, rcYawExpo8)},
    { "thr_mid",                    VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  100 } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, thrMid8)},
    { "thr_expo",                   VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  100 } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, thrExpo8)},
    { "roll_rate",                  VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  CONTROL_RATE_CONFIG_ROLL_PITCH_RATE_MAX } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, rates[ROLL])},
    { "pitch_rate",                 VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  CONTROL_RATE_CONFIG_ROLL_PITCH_RATE_MAX } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, rates[PITCH])},
    { "yaw_rate",                   VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  CONTROL_RATE_CONFIG_YAW_RATE_MAX } , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, rates[YAW])},
    { "tpa_rate",                   VAR_UINT8  | CONTROL_RATE_VALUE, .config.minmax = { 0,  CONTROL_RATE_CONFIG_TPA_MAX} , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, dynThrPID)},
    { "tpa_breakpoint",             VAR_UINT16 | CONTROL_RATE_VALUE, .config.minmax = { PWM_RANGE_MIN,  PWM_RANGE_MAX} , PG_CONTROL_RATE_PROFILES, offsetof(controlRateConfig_t, tpa_breakpoint)},

    { "failsafe_delay",             VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  200 } , PG_FAILSAFE_CONFIG, offsetof(failsafeConfig_t, failsafe_delay)},
    { "failsafe_off_delay",         VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  200 } , PG_FAILSAFE_CONFIG, offsetof(failsafeConfig_t, failsafe_off_delay)},
    { "failsafe_throttle",          VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { PWM_RANGE_MIN,  PWM_RANGE_MAX } , PG_FAILSAFE_CONFIG, offsetof(failsafeConfig_t, failsafe_throttle)},
    { "failsafe_kill_switch",       VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_FAILSAFE_CONFIG, offsetof(failsafeConfig_t, failsafe_kill_switch)},
    { "failsafe_throttle_low_delay",VAR_UINT16 | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  300 } , PG_FAILSAFE_CONFIG, offsetof(failsafeConfig_t, failsafe_throttle_low_delay)},
    { "failsafe_procedure",         VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  1 } , PG_FAILSAFE_CONFIG, offsetof(failsafeConfig_t, failsafe_procedure)},

#ifdef USE_SERVOS
    { "gimbal_mode",                VAR_UINT8  | PROFILE_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_GIMBAL_MODE }, PG_GIMBAL_CONFIG, offsetof(gimbalConfig_t, mode), 0},
#endif

#ifdef BARO
    { "baro_use_median_filter",     VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_OFF_ON } , PG_BARO_CONFIG, offsetof(barometerConfig_t, use_median_filtering)},
    { "baro_hardware",              VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  BARO_MAX } , PG_SENSOR_SELECTION_CONFIG, offsetof(sensorSelectionConfig_t, baro_hardware)},
#endif

#ifdef MAG
    { "mag_hardware",               VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 0,  MAG_MAX } , PG_SENSOR_SELECTION_CONFIG, offsetof(sensorSelectionConfig_t, mag_hardware)},
    { "mag_declination",            VAR_INT16  | PROFILE_VALUE, .config.minmax = { -18000,  18000 } , PG_PROFILE, offsetof(profile_t, mag_declination)},

    { "mag_hold_rate_limit",        VAR_UINT8  | PROFILE_VALUE, .config.minmax = { MAG_HOLD_RATE_LIMIT_MIN,  MAG_HOLD_RATE_LIMIT_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, mag_hold_rate_limit)},
#endif

    { "p_pitch",                    VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, P8[FD_PITCH])},
    { "i_pitch",                    VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, I8[FD_PITCH])},
    { "d_pitch",                    VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, D8[FD_PITCH])},
    { "p_roll",                     VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, P8[FD_ROLL])},
    { "i_roll",                     VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, I8[FD_ROLL])},
    { "d_roll",                     VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, D8[FD_ROLL])},
    { "p_yaw",                      VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, P8[FD_YAW])},
    { "i_yaw",                      VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, I8[FD_YAW])},
    { "d_yaw",                      VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, D8[FD_YAW])},

    { "p_level",                    VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, P8[PIDLEVEL])},
    { "i_level",                    VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, I8[PIDLEVEL])},
    { "d_level",                    VAR_UINT8  | PROFILE_VALUE, .config.minmax = { PID_MIN,  PID_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, D8[PIDLEVEL])},

    { "max_angle_inclination_rll",  VAR_INT16  | PROFILE_VALUE, .config.minmax = {100, 900 } , PG_PID_PROFILE, offsetof(pidProfile_t, max_angle_inclination[FD_ROLL])},
    { "max_angle_inclination_pit",  VAR_INT16  | PROFILE_VALUE, .config.minmax = {100, 900 } , PG_PID_PROFILE, offsetof(pidProfile_t, max_angle_inclination[FD_PITCH])},
    
    { "gyro_soft_lpf_hz",           VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = {0, 200 } , PG_GYRO_CONFIG, offsetof(gyroConfig_t, gyro_soft_lpf_hz)},
    { "acc_soft_lpf_hz",            VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = {0, 200 } , PG_ACC_CONFIG, offsetof(accConfig_t, acc_soft_lpf_hz)},
    { "dterm_lpf_hz",               VAR_UINT8  | PROFILE_VALUE, .config.minmax = {0, 200 }, PG_PID_PROFILE, offsetof(pidProfile_t, dterm_lpf_hz)},
    { "yaw_lpf_hz",                 VAR_UINT8  | PROFILE_VALUE, .config.minmax = {0, 200 }, PG_PID_PROFILE, offsetof(pidProfile_t, yaw_lpf_hz) },

    { "yaw_p_limit",                VAR_UINT16 | PROFILE_VALUE, .config.minmax = { YAW_P_LIMIT_MIN, YAW_P_LIMIT_MAX } , PG_PID_PROFILE, offsetof(pidProfile_t, yaw_p_limit)},

#ifdef BLACKBOX
    { "blackbox_rate_num",          VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 1,  32 } , PG_BLACKBOX_CONFIG, offsetof(blackboxConfig_t, rate_num)},
    { "blackbox_rate_denom",        VAR_UINT8  | MIGRATED_MASTER_VALUE, .config.minmax = { 1,  32 } , PG_BLACKBOX_CONFIG, offsetof(blackboxConfig_t, rate_denom)},
    { "blackbox_device",            VAR_UINT8  | MIGRATED_MASTER_VALUE | MODE_LOOKUP, .config.lookup = { TABLE_BLACKBOX_DEVICE } , PG_BLACKBOX_CONFIG, offsetof(blackboxConfig_t, device)},
#endif

    { "magzero_x",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_MAG_CONFIG, offsetof(magConfig_t, magZero.raw[X]), FLAG_MAG_CALIBRATION_DONE },
    { "magzero_y",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_MAG_CONFIG, offsetof(magConfig_t, magZero.raw[Y]), FLAG_MAG_CALIBRATION_DONE },
    { "magzero_z",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_MAG_CONFIG, offsetof(magConfig_t, magZero.raw[Z]), FLAG_MAG_CALIBRATION_DONE },

    { "acczero_x",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_ACC_CONFIG, offsetof(accConfig_t, accZero.raw[X])},
    { "acczero_y",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_ACC_CONFIG, offsetof(accConfig_t, accZero.raw[Y])},
    { "acczero_z",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_ACC_CONFIG, offsetof(accConfig_t, accZero.raw[Z])},

    { "accgain_x",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_ACC_CONFIG, offsetof(accConfig_t, accGain.raw[X])},
    { "accgain_y",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_ACC_CONFIG, offsetof(accConfig_t, accGain.raw[Y])},
    { "accgain_z",                  VAR_INT16  | MIGRATED_MASTER_VALUE, .config.minmax = { -32768,  32767 } , PG_ACC_CONFIG, offsetof(accConfig_t, accGain.raw[Z])},
};

typedef union {
    int32_t int_value;
    float float_value;
} int_float_value_t;

static void cliSetVar(const clivalue_t *var, const int_float_value_t value);
static void cliPrintVar(const clivalue_t *var, uint32_t full);
static void cliPrint(const char *str);
static void cliPrintf(const char *fmt, ...);
static void cliWrite(uint8_t ch);

static void cliPrompt(void)
{
    cliPrint("\r\n# ");
    bufWriterFlush(cliWriter);
}

static void cliShowParseError(void)
{
    cliPrint("Parse error\r\n");
}

static void cliShowArgumentRangeError(char *name, int min, int max)
{
    cliPrintf("%s must be between %d and %d\r\n", name, min, max);
}

static char *processChannelRangeArgs(char *ptr, channelRange_t *range, uint8_t *validArgumentCount)
{
    int val;

    for (int argIndex = 0; argIndex < 2; argIndex++) {
        ptr = strchr(ptr, ' ');
        if (ptr) {
            val = atoi(++ptr);
            val = CHANNEL_VALUE_TO_STEP(val);
            if (val >= MIN_MODE_RANGE_STEP && val <= MAX_MODE_RANGE_STEP) {
                if (argIndex == 0) {
                    range->startStep = val;
                } else {
                    range->endStep = val;
                }
                (*validArgumentCount)++;
            }
        }
    }

    return ptr;
}

// Check if a string's length is zero
static bool isEmpty(const char *string)
{
    return *string == '\0';
}

static void cliRxFail(char *cmdline)
{
    uint8_t channel;
    char buf[3];

    if (isEmpty(cmdline)) {
        // print out rxConfig failsafe settings
        for (channel = 0; channel < MAX_SUPPORTED_RC_CHANNEL_COUNT; channel++) {
            cliRxFail(itoa(channel, buf, 10));
        }
    } else {
        char *ptr = cmdline;
        channel = atoi(ptr++);
        if ((channel < MAX_SUPPORTED_RC_CHANNEL_COUNT)) {

            rxFailsafeChannelConfiguration_t *channelFailsafeConfiguration = &rxConfig.failsafe_channel_configurations[channel];

            uint16_t value;
            rxFailsafeChannelType_e type = (channel < NON_AUX_CHANNEL_COUNT) ? RX_FAILSAFE_TYPE_FLIGHT : RX_FAILSAFE_TYPE_AUX;
            rxFailsafeChannelMode_e mode = channelFailsafeConfiguration->mode;
            bool requireValue = channelFailsafeConfiguration->mode == RX_FAILSAFE_MODE_SET;

            ptr = strchr(ptr, ' ');
            if (ptr) {
                char *p = strchr(rxFailsafeModeCharacters, *(++ptr));
                if (p) {
                    uint8_t requestedMode = p - rxFailsafeModeCharacters;
                    mode = rxFailsafeModesTable[type][requestedMode];
                } else {
                    mode = RX_FAILSAFE_MODE_INVALID;
                }
                if (mode == RX_FAILSAFE_MODE_INVALID) {
                    cliShowParseError();
                    return;
                }

                requireValue = mode == RX_FAILSAFE_MODE_SET;

                ptr = strchr(ptr, ' ');
                if (ptr) {
                    if (!requireValue) {
                        cliShowParseError();
                        return;
                    }
                    value = atoi(++ptr);
                    value = CHANNEL_VALUE_TO_RXFAIL_STEP(value);
                    if (value > MAX_RXFAIL_RANGE_STEP) {
                        cliPrint("Value out of range\r\n");
                        return;
                    }

                    channelFailsafeConfiguration->step = value;
                } else if (requireValue) {
                    cliShowParseError();
                    return;
                }
                channelFailsafeConfiguration->mode = mode;

            }

            char modeCharacter = rxFailsafeModeCharacters[channelFailsafeConfiguration->mode];

            // triple use of cliPrintf below
            // 1. acknowledge interpretation on command,
            // 2. query current setting on single item,
            // 3. recursive use for full list.

            if (requireValue) {
                cliPrintf("rxfail %u %c %d\r\n",
                    channel,
                    modeCharacter,
                    RXFAIL_STEP_TO_CHANNEL_VALUE(channelFailsafeConfiguration->step)
                );
            } else {
                cliPrintf("rxfail %u %c\r\n",
                    channel,
                    modeCharacter
                );
            }
        } else {
            cliShowArgumentRangeError("channel", 0, MAX_SUPPORTED_RC_CHANNEL_COUNT - 1);
        }
    }
}

static void cliAux(char *cmdline)
{
    int i, val = 0;
    char *ptr;

    if (isEmpty(cmdline)) {
        // print out aux channel settings
        for (i = 0; i < MAX_MODE_ACTIVATION_CONDITION_COUNT; i++) {
            modeActivationCondition_t *mac = &currentProfile->modeActivationConditions[i];
            cliPrintf("aux %u %u %u %u %u\r\n",
                i,
                mac->modeId,
                mac->auxChannelIndex,
                MODE_STEP_TO_CHANNEL_VALUE(mac->range.startStep),
                MODE_STEP_TO_CHANNEL_VALUE(mac->range.endStep)
            );
        }
    } else {
        ptr = cmdline;
        i = atoi(ptr++);
        if (i < MAX_MODE_ACTIVATION_CONDITION_COUNT) {
            modeActivationCondition_t *mac = &currentProfile->modeActivationConditions[i];
            uint8_t validArgumentCount = 0;
            ptr = strchr(ptr, ' ');
            if (ptr) {
                val = atoi(++ptr);
                if (val >= 0 && val < CHECKBOX_ITEM_COUNT) {
                    mac->modeId = val;
                    validArgumentCount++;
                }
            }
            ptr = strchr(ptr, ' ');
            if (ptr) {
                val = atoi(++ptr);
                if (val >= 0 && val < MAX_AUX_CHANNEL_COUNT) {
                    mac->auxChannelIndex = val;
                    validArgumentCount++;
                }
            }
            ptr = processChannelRangeArgs(ptr, &mac->range, &validArgumentCount);

            if (validArgumentCount != 4) {
                memset(mac, 0, sizeof(modeActivationCondition_t));
            }
        } else {
            cliShowArgumentRangeError("index", 0, MAX_MODE_ACTIVATION_CONDITION_COUNT - 1);
        }
    }
}

static void cliSerial(char *cmdline)
{
    int i, val;
    char *ptr;

    if (isEmpty(cmdline)) {
        for (i = 0; i < SERIAL_PORT_COUNT; i++) {
            if (!serialIsPortAvailable(serialConfig.portConfigs[i].identifier)) {
                continue;
            };
            cliPrintf("serial %d %d %ld %ld %ld %ld\r\n" ,
                serialConfig.portConfigs[i].identifier,
                serialConfig.portConfigs[i].functionMask,
                baudRates[serialConfig.portConfigs[i].msp_baudrateIndex],
                baudRates[serialConfig.portConfigs[i].gps_baudrateIndex],
                baudRates[serialConfig.portConfigs[i].telemetry_baudrateIndex],
                baudRates[serialConfig.portConfigs[i].blackbox_baudrateIndex]
            );
        }
        return;
    }

    serialPortConfig_t portConfig;
    memset(&portConfig, 0 , sizeof(portConfig));

    serialPortConfig_t *currentConfig;

    uint8_t validArgumentCount = 0;

    ptr = cmdline;

    val = atoi(ptr++);
    currentConfig = serialFindPortConfiguration(val);
    if (currentConfig) {
        portConfig.identifier = val;
        validArgumentCount++;
    }

    ptr = strchr(ptr, ' ');
    if (ptr) {
        val = atoi(++ptr);
        portConfig.functionMask = val & 0xFFFF;
        validArgumentCount++;
    }

    for (i = 0; i < 4; i ++) {
        ptr = strchr(ptr, ' ');
        if (!ptr) {
            break;
        }

        val = atoi(++ptr);

        uint8_t baudRateIndex = lookupBaudRateIndex(val);
        if (baudRates[baudRateIndex] != (uint32_t) val) {
            break;
        }

        switch(i) {
            case 0:
                if (baudRateIndex < BAUD_9600 || baudRateIndex > BAUD_115200) {
                    continue;
                }
                portConfig.msp_baudrateIndex = baudRateIndex;
                break;
            case 1:
                if (baudRateIndex < BAUD_9600 || baudRateIndex > BAUD_115200) {
                    continue;
                }
                portConfig.gps_baudrateIndex = baudRateIndex;
                break;
            case 2:
                if (baudRateIndex != BAUD_AUTO && baudRateIndex > BAUD_115200) {
                    continue;
                }
                portConfig.telemetry_baudrateIndex = baudRateIndex;
                break;
            case 3:
                if (baudRateIndex < BAUD_19200 || baudRateIndex > BAUD_250000) {
                    continue;
                }
                portConfig.blackbox_baudrateIndex = baudRateIndex;
                break;
        }

        validArgumentCount++;
    }

    if (validArgumentCount < 6) {
        cliShowParseError();
        return;
    }

    memcpy(currentConfig, &portConfig, sizeof(portConfig));

}

static void cliAdjustmentRange(char *cmdline)
{
    int i, val = 0;
    char *ptr;

    if (isEmpty(cmdline)) {
        // print out adjustment ranges channel settings
        for (i = 0; i < MAX_ADJUSTMENT_RANGE_COUNT; i++) {
            adjustmentRange_t *ar = &currentProfile->adjustmentRanges[i];
            cliPrintf("adjrange %u %u %u %u %u %u %u\r\n",
                i,
                ar->adjustmentIndex,
                ar->auxChannelIndex,
                MODE_STEP_TO_CHANNEL_VALUE(ar->range.startStep),
                MODE_STEP_TO_CHANNEL_VALUE(ar->range.endStep),
                ar->adjustmentFunction,
                ar->auxSwitchChannelIndex
            );
        }
    } else {
        ptr = cmdline;
        i = atoi(ptr++);
        if (i < MAX_ADJUSTMENT_RANGE_COUNT) {
            adjustmentRange_t *ar = &currentProfile->adjustmentRanges[i];
            uint8_t validArgumentCount = 0;

            ptr = strchr(ptr, ' ');
            if (ptr) {
                val = atoi(++ptr);
                if (val >= 0 && val < MAX_SIMULTANEOUS_ADJUSTMENT_COUNT) {
                    ar->adjustmentIndex = val;
                    validArgumentCount++;
                }
            }
            ptr = strchr(ptr, ' ');
            if (ptr) {
                val = atoi(++ptr);
                if (val >= 0 && val < MAX_AUX_CHANNEL_COUNT) {
                    ar->auxChannelIndex = val;
                    validArgumentCount++;
                }
            }

            ptr = processChannelRangeArgs(ptr, &ar->range, &validArgumentCount);

            ptr = strchr(ptr, ' ');
            if (ptr) {
                val = atoi(++ptr);
                if (val >= 0 && val < ADJUSTMENT_FUNCTION_COUNT) {
                    ar->adjustmentFunction = val;
                    validArgumentCount++;
                }
            }
            ptr = strchr(ptr, ' ');
            if (ptr) {
                val = atoi(++ptr);
                if (val >= 0 && val < MAX_AUX_CHANNEL_COUNT) {
                    ar->auxSwitchChannelIndex = val;
                    validArgumentCount++;
                }
            }

            if (validArgumentCount != 6) {
                memset(ar, 0, sizeof(adjustmentRange_t));
                cliShowParseError();
            }
        } else {
            cliShowArgumentRangeError("index", 0, MAX_ADJUSTMENT_RANGE_COUNT - 1);
        }
    }
}

static void cliMotorMix(char *cmdline)
{
#ifdef USE_QUAD_MIXER_ONLY
    UNUSED(cmdline);
#else
    int i, check = 0;
    int num_motors = 0;
    uint8_t len;
    char ftoaBuffer[FTOA_BUFFER_SIZE];
    char *ptr;

    if (isEmpty(cmdline)) {
        cliPrint("Motor\tThr\tRoll\tPitch\tYaw\r\n");
        for (i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
            if (customMotorMixer[i].throttle == 0.0f)
                break;
            num_motors++;
            cliPrintf("#%d:\t", i);
            cliPrintf("%s\t", ftoa(customMotorMixer[i].throttle, ftoaBuffer));
            cliPrintf("%s\t", ftoa(customMotorMixer[i].roll, ftoaBuffer));
            cliPrintf("%s\t", ftoa(customMotorMixer[i].pitch, ftoaBuffer));
            cliPrintf("%s\r\n", ftoa(customMotorMixer[i].yaw, ftoaBuffer));
        }
        return;
    } else if (strncasecmp(cmdline, "reset", 5) == 0) {
        // erase custom mixer
        for (i = 0; i < MAX_SUPPORTED_MOTORS; i++)
            customMotorMixer[i].throttle = 0.0f;
    } else if (strncasecmp(cmdline, "load", 4) == 0) {
        ptr = strchr(cmdline, ' ');
        if (ptr) {
            len = strlen(++ptr);
            for (i = 0; ; i++) {
                if (mixerNames[i] == NULL) {
                    cliPrint("Invalid name\r\n");
                    break;
                }
                if (strncasecmp(ptr, mixerNames[i], len) == 0) {
                    mixerLoadMix(i, customMotorMixer);
                    cliPrintf("Loaded %s\r\n", mixerNames[i]);
                    cliMotorMix("");
                    break;
                }
            }
        }
    } else {
        ptr = cmdline;
        i = atoi(ptr); // get motor number
        if (i < MAX_SUPPORTED_MOTORS) {
            ptr = strchr(ptr, ' ');
            if (ptr) {
                customMotorMixer[i].throttle = fastA2F(++ptr);
                check++;
            }
            ptr = strchr(ptr, ' ');
            if (ptr) {
                customMotorMixer[i].roll = fastA2F(++ptr);
                check++;
            }
            ptr = strchr(ptr, ' ');
            if (ptr) {
                customMotorMixer[i].pitch = fastA2F(++ptr);
                check++;
            }
            ptr = strchr(ptr, ' ');
            if (ptr) {
                customMotorMixer[i].yaw = fastA2F(++ptr);
                check++;
            }
            if (check != 4) {
                cliShowParseError();
            } else {
                cliMotorMix("");
            }
        } else {
            cliShowArgumentRangeError("index", 0, MAX_SUPPORTED_MOTORS - 1);
        }
    }
#endif
}

static void cliRxRange(char *cmdline)
{
    int i, validArgumentCount = 0;
    char *ptr;

    if (isEmpty(cmdline)) {
        for (i = 0; i < NON_AUX_CHANNEL_COUNT; i++) {
            rxChannelRangeConfiguration_t *channelRangeConfiguration = &rxConfig.channelRanges[i];
            cliPrintf("rxrange %u %u %u\r\n", i, channelRangeConfiguration->min, channelRangeConfiguration->max);
        }
    } else if (strcasecmp(cmdline, "reset") == 0) {
        resetAllRxChannelRangeConfigurations(rxConfig.channelRanges);
    } else {
        ptr = cmdline;
        i = atoi(ptr);
        if (i >= 0 && i < NON_AUX_CHANNEL_COUNT) {
            int rangeMin, rangeMax;

            ptr = strchr(ptr, ' ');
            if (ptr) {
                rangeMin = atoi(++ptr);
                validArgumentCount++;
            }

            ptr = strchr(ptr, ' ');
            if (ptr) {
                rangeMax = atoi(++ptr);
                validArgumentCount++;
            }

            if (validArgumentCount != 2) {
                cliShowParseError();
            } else if (rangeMin < PWM_PULSE_MIN || rangeMin > PWM_PULSE_MAX || rangeMax < PWM_PULSE_MIN || rangeMax > PWM_PULSE_MAX) {
                cliShowParseError();
            } else {
                rxChannelRangeConfiguration_t *channelRangeConfiguration = &rxConfig.channelRanges[i];
                channelRangeConfiguration->min = rangeMin;
                channelRangeConfiguration->max = rangeMax;
            }
        } else {
            cliShowArgumentRangeError("channel", 0, NON_AUX_CHANNEL_COUNT - 1);
        }
    }
}

#ifdef LED_STRIP
static void cliLed(char *cmdline)
{
    int i;
    char *ptr;
    char ledConfigBuffer[20];

    if (isEmpty(cmdline)) {
        for (i = 0; i < MAX_LED_STRIP_LENGTH; i++) {
            generateLedConfig(i, ledConfigBuffer, sizeof(ledConfigBuffer));
            cliPrintf("led %u %s\r\n", i, ledConfigBuffer);
        }
    } else {
        ptr = cmdline;
        i = atoi(ptr);
        if (i < MAX_LED_STRIP_LENGTH) {
            ptr = strchr(cmdline, ' ');
            if (!parseLedStripConfig(i, ++ptr)) {
                cliShowParseError();
            }
        } else {
            cliShowArgumentRangeError("index", 0, MAX_LED_STRIP_LENGTH - 1);
        }
    }
}

static void cliColor(char *cmdline)
{
    int i;
    char *ptr;

    if (isEmpty(cmdline)) {
        for (i = 0; i < CONFIGURABLE_COLOR_COUNT; i++) {
            cliPrintf("color %u %d,%u,%u\r\n",
                i,
                colors[i].h,
                colors[i].s,
                colors[i].v
            );
        }
    } else {
        ptr = cmdline;
        i = atoi(ptr);
        if (i < CONFIGURABLE_COLOR_COUNT) {
            ptr = strchr(cmdline, ' ');
            if (!parseColor(i, ++ptr)) {
                cliShowParseError();
            }
        } else {
            cliShowArgumentRangeError("index", 0, CONFIGURABLE_COLOR_COUNT - 1);
        }
    }
}
#endif

#ifdef USE_SERVOS
static void cliServo(char *cmdline)
{
    enum { SERVO_ARGUMENT_COUNT = 8 };
    int16_t arguments[SERVO_ARGUMENT_COUNT];

    servoParam_t *servo;

    int i;
    char *ptr;

    if (isEmpty(cmdline)) {
        // print out servo settings
        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            servo = &currentProfile->servoConf[i];

            cliPrintf("servo %u %d %d %d %d %d %d %d\r\n",
                i,
                servo->min,
                servo->max,
                servo->middle,
                servo->angleAtMin,
                servo->angleAtMax,
                servo->rate,
                servo->forwardFromChannel
            );
        }
    } else {
        int validArgumentCount = 0;

        ptr = cmdline;

        // Command line is integers (possibly negative) separated by spaces, no other characters allowed.

        // If command line doesn't fit the format, don't modify the config
        while (*ptr) {
            if (*ptr == '-' || (*ptr >= '0' && *ptr <= '9')) {
                if (validArgumentCount >= SERVO_ARGUMENT_COUNT) {
                    cliShowParseError();
                    return;
                }

                arguments[validArgumentCount++] = atoi(ptr);

                do {
                    ptr++;
                } while (*ptr >= '0' && *ptr <= '9');
            } else if (*ptr == ' ') {
                ptr++;
            } else {
                cliShowParseError();
                return;
            }
        }

        enum {INDEX = 0, MIN, MAX, MIDDLE, ANGLE_AT_MIN, ANGLE_AT_MAX, RATE, FORWARD};

        i = arguments[INDEX];

        // Check we got the right number of args and the servo index is correct (don't validate the other values)
        if (validArgumentCount != SERVO_ARGUMENT_COUNT || i < 0 || i >= MAX_SUPPORTED_SERVOS) {
            cliShowParseError();
            return;
        }

        servo = &currentProfile->servoConf[i];

        if (
            arguments[MIN] < PWM_PULSE_MIN || arguments[MIN] > PWM_PULSE_MAX ||
            arguments[MAX] < PWM_PULSE_MIN || arguments[MAX] > PWM_PULSE_MAX ||
            arguments[MIDDLE] < arguments[MIN] || arguments[MIDDLE] > arguments[MAX] ||
            arguments[MIN] > arguments[MAX] || arguments[MAX] < arguments[MIN] ||
            arguments[RATE] < -100 || arguments[RATE] > 100 ||
            arguments[FORWARD] >= MAX_SUPPORTED_RC_CHANNEL_COUNT ||
            arguments[ANGLE_AT_MIN] < 0 || arguments[ANGLE_AT_MIN] > 180 ||
            arguments[ANGLE_AT_MAX] < 0 || arguments[ANGLE_AT_MAX] > 180
        ) {
            cliShowParseError();
            return;
        }

        servo->min = arguments[1];
        servo->max = arguments[2];
        servo->middle = arguments[3];
        servo->angleAtMin = arguments[4];
        servo->angleAtMax = arguments[5];
        servo->rate = arguments[6];
        servo->forwardFromChannel = arguments[7];
    }
}
#endif

#ifdef USE_SERVOS
static void cliServoMix(char *cmdline)
{
    int i;
    uint8_t len;
    char *ptr;
    int args[8], check = 0;
    len = strlen(cmdline);

    if (len == 0) {

        cliPrint("Rule\tServo\tSource\tRate\tSpeed\tMin\tMax\tBox\r\n");

        for (i = 0; i < MAX_SERVO_RULES; i++) {
            if (customServoMixer[i].rate == 0)
                break;

            cliPrintf("#%d:\t%d\t%d\t%d\t%d\t%d\t%d\t%d\r\n",
                i,
                customServoMixer[i].targetChannel,
                customServoMixer[i].inputSource,
                customServoMixer[i].rate,
                customServoMixer[i].speed,
                customServoMixer[i].min,
                customServoMixer[i].max,
                customServoMixer[i].box
            );
        }
        cliPrintf("\r\n");
        return;
    } else if (strncasecmp(cmdline, "reset", 5) == 0) {
        // erase custom mixer
        memset(customServoMixer, 0, sizeof(customServoMixer));
        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            currentProfile->servoConf[i].reversedSources = 0;
        }
    } else if (strncasecmp(cmdline, "load", 4) == 0) {
        ptr = strchr(cmdline, ' ');
        if (ptr) {
            len = strlen(++ptr);
            for (i = 0; ; i++) {
                if (mixerNames[i] == NULL) {
                    cliPrintf("Invalid name\r\n");
                    break;
                }
                if (strncasecmp(ptr, mixerNames[i], len) == 0) {
                    servoMixerLoadMix(i, customServoMixer);
                    cliPrintf("Loaded %s\r\n", mixerNames[i]);
                    cliServoMix("");
                    break;
                }
            }
        }
    } else if (strncasecmp(cmdline, "reverse", 7) == 0) {
        enum {SERVO = 0, INPUT, REVERSE, ARGS_COUNT};
        int servoIndex, inputSource;
        ptr = strchr(cmdline, ' ');

        len = strlen(ptr);
        if (len == 0) {
            cliPrintf("s");
            for (inputSource = 0; inputSource < INPUT_SOURCE_COUNT; inputSource++)
                cliPrintf("\ti%d", inputSource);
            cliPrintf("\r\n");

            for (servoIndex = 0; servoIndex < MAX_SUPPORTED_SERVOS; servoIndex++) {
                cliPrintf("%d", servoIndex);
                for (inputSource = 0; inputSource < INPUT_SOURCE_COUNT; inputSource++)
                    cliPrintf("\t%s  ", (currentProfile->servoConf[servoIndex].reversedSources & (1 << inputSource)) ? "r" : "n");
                cliPrintf("\r\n");
            }
            return;
        }

        ptr = strtok(ptr, " ");
        while (ptr != NULL && check < ARGS_COUNT - 1) {
            args[check++] = atoi(ptr);
            ptr = strtok(NULL, " ");
        }

        if (ptr == NULL || check != ARGS_COUNT - 1) {
            cliShowParseError();
            return;
        }

        if (args[SERVO] >= 0 && args[SERVO] < MAX_SUPPORTED_SERVOS
                && args[INPUT] >= 0 && args[INPUT] < INPUT_SOURCE_COUNT
                && (*ptr == 'r' || *ptr == 'n')) {
            if (*ptr == 'r')
                currentProfile->servoConf[args[SERVO]].reversedSources |= 1 << args[INPUT];
            else
                currentProfile->servoConf[args[SERVO]].reversedSources &= ~(1 << args[INPUT]);
        } else
            cliShowParseError();

        cliServoMix("reverse");
    } else {
        enum {RULE = 0, TARGET, INPUT, RATE, SPEED, MIN, MAX, BOX, ARGS_COUNT};
        ptr = strtok(cmdline, " ");
        while (ptr != NULL && check < ARGS_COUNT) {
            args[check++] = atoi(ptr);
            ptr = strtok(NULL, " ");
        }

        if (ptr != NULL || check != ARGS_COUNT) {
            cliShowParseError();
            return;
        }

        i = args[RULE];
        if (i >= 0 && i < MAX_SERVO_RULES &&
            args[TARGET] >= 0 && args[TARGET] < MAX_SUPPORTED_SERVOS &&
            args[INPUT] >= 0 && args[INPUT] < INPUT_SOURCE_COUNT &&
            args[RATE] >= -100 && args[RATE] <= 100 &&
            args[SPEED] >= 0 && args[SPEED] <= MAX_SERVO_SPEED &&
            args[MIN] >= 0 && args[MIN] <= 100 &&
            args[MAX] >= 0 && args[MAX] <= 100 && args[MIN] < args[MAX] &&
            args[BOX] >= 0 && args[BOX] <= MAX_SERVO_BOXES) {
            customServoMixer[i].targetChannel = args[TARGET];
            customServoMixer[i].inputSource = args[INPUT];
            customServoMixer[i].rate = args[RATE];
            customServoMixer[i].speed = args[SPEED];
            customServoMixer[i].min = args[MIN];
            customServoMixer[i].max = args[MAX];
            customServoMixer[i].box = args[BOX];
            cliServoMix("");
        } else {
            cliShowParseError();
        }
    }
}
#endif


#ifdef USE_FLASHFS

static void cliFlashInfo(char *cmdline)
{
    const flashGeometry_t *layout = flashfsGetGeometry();

    UNUSED(cmdline);

    cliPrintf("Flash sectors=%u, sectorSize=%u, pagesPerSector=%u, pageSize=%u, totalSize=%u, usedSize=%u\r\n",
            layout->sectors, layout->sectorSize, layout->pagesPerSector, layout->pageSize, layout->totalSize, flashfsGetOffset());
}

static void cliFlashErase(char *cmdline)
{
    UNUSED(cmdline);

    cliPrintf("Erasing...\r\n");
    flashfsEraseCompletely();

    while (!flashfsIsReady()) {
        delay(100);
    }

    cliPrintf("Done.\r\n");
}

#ifdef USE_FLASH_TOOLS

static void cliFlashWrite(char *cmdline)
{
    uint32_t address = atoi(cmdline);
    char *text = strchr(cmdline, ' ');

    if (!text) {
        cliShowParseError();
    } else {
        flashfsSeekAbs(address);
        flashfsWrite((uint8_t*)text, strlen(text), true);
        flashfsFlushSync();

        cliPrintf("Wrote %u bytes at %u.\r\n", strlen(text), address);
    }
}

static void cliFlashRead(char *cmdline)
{
    uint32_t address = atoi(cmdline);
    uint32_t length;
    int i;

    uint8_t buffer[32];

    char *nextArg = strchr(cmdline, ' ');

    if (!nextArg) {
        cliShowParseError();
    } else {
        length = atoi(nextArg);

        cliPrintf("Reading %u bytes at %u:\r\n", length, address);

        while (length > 0) {
            int bytesRead;

            bytesRead = flashfsReadAbs(address, buffer, length < sizeof(buffer) ? length : sizeof(buffer));

            for (i = 0; i < bytesRead; i++) {
                cliWrite(buffer[i]);
            }

            length -= bytesRead;
            address += bytesRead;

            if (bytesRead == 0) {
                //Assume we reached the end of the volume or something fatal happened
                break;
            }
        }
        cliPrintf("\r\n");
    }
}

#endif
#endif

static void dumpValues(uint16_t valueSection)
{
    uint32_t i;
    const clivalue_t *value;
    for (i = 0; i < ARRAYLEN(valueTable); i++) {
        value = &valueTable[i];

        if ((value->type & VALUE_SECTION_MASK) != valueSection) {
            continue;
        }

        cliPrintf("set %s = ", valueTable[i].name);
        cliPrintVar(value, 0);
        cliPrint("\r\n");
    }
}

typedef enum {
    DUMP_MASTER = (1 << 0),
    DUMP_PROFILE = (1 << 1),
    DUMP_CONTROL_RATE_PROFILE = (1 << 2)
} dumpFlags_e;

#define DUMP_ALL (DUMP_MASTER | DUMP_PROFILE | DUMP_CONTROL_RATE_PROFILE)


static const char* const sectionBreak = "\r\n";

#define printSectionBreak() cliPrintf((char *)sectionBreak)

static void cliDump(char *cmdline)
{
    unsigned int i;
    char buf[16];
    uint32_t mask;

#ifndef USE_QUAD_MIXER_ONLY
    float thr, roll, pitch, yaw;
#endif

    uint8_t dumpMask = DUMP_ALL;
    if (strcasecmp(cmdline, "master") == 0) {
        dumpMask = DUMP_MASTER; // only
    }
    if (strcasecmp(cmdline, "profile") == 0) {
        dumpMask = DUMP_PROFILE; // only
    }
    if (strcasecmp(cmdline, "rates") == 0) {
        dumpMask = DUMP_CONTROL_RATE_PROFILE; // only
    }

    if (dumpMask & DUMP_MASTER) {

        cliPrint("\r\n# version\r\n");
        cliVersion(NULL);

        cliPrint("\r\n# pflags\r\n");
        cliPFlags("");

        cliPrint("\r\n# dump master\r\n");
        cliPrint("\r\n# mixer\r\n");

#ifndef USE_QUAD_MIXER_ONLY
        cliPrintf("mixer %s\r\n", mixerNames[mixerConfig.mixerMode - 1]);

        cliPrintf("mmix reset\r\n");

        for (i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
            if (customMotorMixer[i].throttle == 0.0f)
                break;
            thr = customMotorMixer[i].throttle;
            roll = customMotorMixer[i].roll;
            pitch = customMotorMixer[i].pitch;
            yaw = customMotorMixer[i].yaw;
            cliPrintf("mmix %d", i);
            if (thr < 0)
                cliWrite(' ');
            cliPrintf("%s", ftoa(thr, buf));
            if (roll < 0)
                cliWrite(' ');
            cliPrintf("%s", ftoa(roll, buf));
            if (pitch < 0)
                cliWrite(' ');
            cliPrintf("%s", ftoa(pitch, buf));
            if (yaw < 0)
                cliWrite(' ');
            cliPrintf("%s\r\n", ftoa(yaw, buf));
        }

        // print custom servo mixer if exists
        cliPrintf("smix reset\r\n");

        for (i = 0; i < MAX_SERVO_RULES; i++) {

            if (customServoMixer[i].rate == 0)
                break;

            cliPrintf("smix %d %d %d %d %d %d %d %d\r\n",
                i,
                customServoMixer[i].targetChannel,
                customServoMixer[i].inputSource,
                customServoMixer[i].rate,
                customServoMixer[i].speed,
                customServoMixer[i].min,
                customServoMixer[i].max,
                customServoMixer[i].box
            );
        }

#endif

        cliPrint("\r\n\r\n# feature\r\n");

        mask = featureMask();
        for (i = 0; ; i++) { // disable all feature first
            if (featureNames[i] == NULL)
                break;
            cliPrintf("feature -%s\r\n", featureNames[i]);
        }
        for (i = 0; ; i++) {  // reenable what we want.
            if (featureNames[i] == NULL)
                break;
            if (mask & (1 << i))
                cliPrintf("feature %s\r\n", featureNames[i]);
        }


#ifdef BEEPER
        cliPrint("\r\n\r\n# beeper\r\n");

        uint8_t beeperCount = beeperTableEntryCount();
        mask = getBeeperOffMask();
        for (int i = 0; i < (beeperCount-2); i++) {
            if (mask & (1 << i))
                cliPrintf("beeper -%s\r\n", beeperNameForTableIndex(i));
            else
                cliPrintf("beeper %s\r\n", beeperNameForTableIndex(i));
        }
#endif


        cliPrint("\r\n\r\n# map\r\n");

        for (i = 0; i < 8; i++)
            buf[rxConfig.rcmap[i]] = rcChannelLetters[i];
        buf[i] = '\0';
        cliPrintf("map %s\r\n", buf);

        cliPrint("\r\n\r\n# serial\r\n");
        cliSerial("");

#ifdef LED_STRIP
        cliPrint("\r\n\r\n# led\r\n");
        cliLed("");

        cliPrint("\r\n\r\n# color\r\n");
        cliColor("");
#endif
        printSectionBreak();
        dumpValues(MASTER_VALUE);
        dumpValues(MIGRATED_MASTER_VALUE);

        cliPrint("\r\n# rxfail\r\n");
        cliRxFail("");
    }

    if (dumpMask & DUMP_PROFILE) {
        cliPrint("\r\n# dump profile\r\n");

        cliPrint("\r\n# profile\r\n");
        cliProfile("");

        cliPrint("\r\n# aux\r\n");

        cliAux("");

        cliPrint("\r\n# adjrange\r\n");

        cliAdjustmentRange("");

        cliPrintf("\r\n# rxrange\r\n");

        cliRxRange("");

#ifdef USE_SERVOS
        cliPrint("\r\n# servo\r\n");

        cliServo("");

        // print servo directions
        unsigned int channel;

        for (i = 0; i < MAX_SUPPORTED_SERVOS; i++) {
            for (channel = 0; channel < INPUT_SOURCE_COUNT; channel++) {
                if (servoDirection(i, channel) < 0) {
                    cliPrintf("smix reverse %d %d r\r\n", i , channel);
                }
            }
        }
#endif

        printSectionBreak();

        dumpValues(PROFILE_VALUE);
    }

    if (dumpMask & DUMP_CONTROL_RATE_PROFILE) {
        cliPrint("\r\n# dump rates\r\n");

        cliPrint("\r\n# rateprofile\r\n");
        cliRateProfile("");

        printSectionBreak();

        dumpValues(CONTROL_RATE_VALUE);
    }
}

void cliEnter(serialPort_t *serialPort)
{
    cliMode = 1;
    cliPort = serialPort;
    setPrintfSerialPort(cliPort);
    cliWriter = bufWriterInit(cliWriteBuffer, sizeof(cliWriteBuffer),
                              (bufWrite_t)serialWriteBufShim, serialPort);

    cliPrint("\r\nEntering CLI Mode, type 'exit' to return, or 'help'\r\n");
    cliPrompt();
    ENABLE_ARMING_FLAG(PREVENT_ARMING);
}

static void cliExit(char *cmdline)
{
    UNUSED(cmdline);

    cliPrint("\r\nLeaving CLI mode, unsaved changes lost.\r\n");
    bufWriterFlush(cliWriter);

    *cliBuffer = '\0';
    bufferIndex = 0;
    cliMode = 0;
    // incase a motor was left running during motortest, clear it here
    mixerResetDisarmedMotors();
    cliReboot();

    cliWriter = NULL;
}

static void cliFeature(char *cmdline)
{
    uint32_t i;
    uint32_t len;
    uint32_t mask;

    len = strlen(cmdline);
    mask = featureMask();

    if (len == 0) {
        cliPrint("Enabled: ");
        for (i = 0; ; i++) {
            if (featureNames[i] == NULL)
                break;
            if (mask & (1 << i))
                cliPrintf("%s ", featureNames[i]);
        }
        cliPrint("\r\n");
    } else if (strncasecmp(cmdline, "list", len) == 0) {
        cliPrint("Available: ");
        for (i = 0; ; i++) {
            if (featureNames[i] == NULL)
                break;
            cliPrintf("%s ", featureNames[i]);
        }
        cliPrint("\r\n");
        return;
    } else {
        bool remove = false;
        if (cmdline[0] == '-') {
            // remove feature
            remove = true;
            cmdline++; // skip over -
            len--;
        }

        for (i = 0; ; i++) {
            if (featureNames[i] == NULL) {
                cliPrint("Invalid name\r\n");
                break;
            }

            if (strncasecmp(cmdline, featureNames[i], len) == 0) {

                mask = 1 << i;
#ifndef GPS
                if (mask & FEATURE_GPS) {
                    cliPrint("unavailable\r\n");
                    break;
                }
#endif
#ifndef SONAR
                if (mask & FEATURE_SONAR) {
                    cliPrint("unavailable\r\n");
                    break;
                }
#endif
                if (remove) {
                    featureClear(mask);
                    cliPrint("Disabled");
                } else {
                    featureSet(mask);
                    cliPrint("Enabled");
                }
                cliPrintf(" %s\r\n", featureNames[i]);
                break;
            }
        }
    }
}

#ifdef BEEPER
static void cliBeeper(char *cmdline)
{
    uint32_t i;
    uint32_t len = strlen(cmdline);;
    uint8_t beeperCount = beeperTableEntryCount();
    uint32_t mask = getBeeperOffMask();

    if (len == 0) {
        cliPrintf("Disabled:");
        for (int i = 0; ; i++) {
        	if (i == beeperCount-2){
                if (mask == 0)
                	cliPrint("  none");
        		break;
        	}
            if (mask & (1 << i))
                cliPrintf("  %s", beeperNameForTableIndex(i));
        }
        cliPrint("\r\n");
    } else if (strncasecmp(cmdline, "list", len) == 0) {
        cliPrint("Available:");
        for (i = 0; i < beeperCount; i++)
            cliPrintf("  %s", beeperNameForTableIndex(i));
        cliPrint("\r\n");
        return;
    } else {
        bool remove = false;
        if (cmdline[0] == '-') {
            remove = true;     // this is for beeper OFF condition
            cmdline++;
            len--;
        }

        for (i = 0; ; i++) {
            if (i == beeperCount) {
                cliPrint("Invalid name\r\n");
                break;
            }
            if (strncasecmp(cmdline, beeperNameForTableIndex(i), len) == 0) {
                if (remove) { // beeper off
                    if (i == BEEPER_ALL-1)
                        beeperOffSetAll(beeperCount-2);
                    else
                    	if (i == BEEPER_PREFERENCE-1)
                            setBeeperOffMask(getPreferedBeeperOffMask());
                        else {
                            mask = 1 << i;
                            beeperOffSet(mask);
                        }
                    cliPrint("Disabled");
                }
                else { // beeper on
                    if (i == BEEPER_ALL-1)
                        beeperOffClearAll();
                    else
                    	if (i == BEEPER_PREFERENCE-1)
                            setPreferedBeeperOffMask(getBeeperOffMask());
                        else {
                            mask = 1 << i;
                            beeperOffClear(mask);
                        }
                    cliPrint("Enabled");
                }
            cliPrintf(" %s\r\n", beeperNameForTableIndex(i));
            break;
            }
        }
    }
}
#endif


#ifdef GPS
static void cliGpsPassthrough(char *cmdline)
{
    UNUSED(cmdline);

    gpsEnablePassthrough(cliPort);
}
#endif

static void cliHelp(char *cmdline)
{
    uint32_t i = 0;

    UNUSED(cmdline);

    for (i = 0; i < CMD_COUNT; i++) {
        cliPrint(cmdTable[i].name);
#ifndef SKIP_CLI_COMMAND_HELP
        if (cmdTable[i].description) {
            cliPrintf(" - %s", cmdTable[i].description);
        }
        if (cmdTable[i].args) {
            cliPrintf("\r\n\t%s", cmdTable[i].args);
        }
#endif
        cliPrint("\r\n");
    }
}

static void cliMap(char *cmdline)
{
    uint32_t len;
    uint32_t i;
    char out[9];

    len = strlen(cmdline);

    if (len == 8) {
        // uppercase it
        for (i = 0; i < 8; i++)
            cmdline[i] = toupper((unsigned char)cmdline[i]);
        for (i = 0; i < 8; i++) {
            if (strchr(rcChannelLetters, cmdline[i]) && !strchr(cmdline + i + 1, cmdline[i]))
                continue;
            cliShowParseError();
            return;
        }
        parseRcChannels(cmdline, &rxConfig);
    }
    cliPrint("Map: ");
    for (i = 0; i < 8; i++)
        out[rxConfig.rcmap[i]] = rcChannelLetters[i];
    out[i] = '\0';
    cliPrintf("%s\r\n", out);
}

#ifndef USE_QUAD_MIXER_ONLY
static void cliMixer(char *cmdline)
{
    int i;
    int len;

    len = strlen(cmdline);

    if (len == 0) {
        cliPrintf("Mixer: %s\r\n", mixerNames[mixerConfig.mixerMode - 1]);
        return;
    } else if (strncasecmp(cmdline, "list", len) == 0) {
        cliPrint("Available mixers: ");
        for (i = 0; ; i++) {
            if (mixerNames[i] == NULL)
                break;
            cliPrintf("%s ", mixerNames[i]);
        }
        cliPrint("\r\n");
        return;
    }

    for (i = 0; ; i++) {
        if (mixerNames[i] == NULL) {
            cliPrint("Invalid name\r\n");
            return;
        }
        if (strncasecmp(cmdline, mixerNames[i], len) == 0) {
            mixerConfig.mixerMode = i + 1;
            break;
        }
    }

    cliMixer("");
}
#endif

static void cliMotor(char *cmdline)
{
    int motor_index = 0;
    int motor_value = 0;
    int index = 0;
    char *pch = NULL;
    char *saveptr;

    if (isEmpty(cmdline)) {
        cliShowParseError();
        return;
    }

    pch = strtok_r(cmdline, " ", &saveptr);
    while (pch != NULL) {
        switch (index) {
            case 0:
                motor_index = atoi(pch);
                break;
            case 1:
                motor_value = atoi(pch);
                break;
        }
        index++;
        pch = strtok_r(NULL, " ", &saveptr);
    }

    if (motor_index < 0 || motor_index >= MAX_SUPPORTED_MOTORS) {
        cliShowArgumentRangeError("index", 0, MAX_SUPPORTED_MOTORS - 1);
        return;
    }

    if (index == 2) {
        if (motor_value < PWM_RANGE_MIN || motor_value > PWM_RANGE_MAX) {
            cliShowArgumentRangeError("value", 1000, 2000);
            return;
        } else {
            motor_disarmed[motor_index] = motor_value;
        }
    }

    cliPrintf("motor %d: %d\r\n", motor_index, motor_disarmed[motor_index]);
}

static void cliPlaySound(char *cmdline)
{
#if FLASH_SIZE <= 64
    UNUSED(cmdline);
#else
    int i;
    const char *name;
    static int lastSoundIdx = -1;

    if (isEmpty(cmdline)) {
        i = lastSoundIdx + 1;     //next sound index
        if ((name=beeperNameForTableIndex(i)) == NULL) {
            while (true) {   //no name for index; try next one
                if (++i >= beeperTableEntryCount())
                    i = 0;   //if end then wrap around to first entry
                if ((name=beeperNameForTableIndex(i)) != NULL)
                    break;   //if name OK then play sound below
                if (i == lastSoundIdx + 1) {     //prevent infinite loop
                    cliPrintf("Error playing sound\r\n");
                    return;
                }
            }
        }
    } else {       //index value was given
        i = atoi(cmdline);
        if ((name=beeperNameForTableIndex(i)) == NULL) {
            cliPrintf("No sound for index %d\r\n", i);
            return;
        }
    }
    lastSoundIdx = i;
    beeperSilence();
    cliPrintf("Playing sound %d: %s\r\n", i, name);
    beeper(beeperModeForTableIndex(i));
#endif
}

static void cliProfile(char *cmdline)
{
    int i;

    if (isEmpty(cmdline)) {
        cliPrintf("profile %d\r\n", getCurrentProfile());
        return;
    } else {
        i = atoi(cmdline);
        if (i >= 0 && i < MAX_PROFILE_COUNT) {
            changeProfile(i);
            cliProfile("");
        }
    }
}

static void cliRateProfile(char *cmdline)
{
    int i;

    if (isEmpty(cmdline)) {
        cliPrintf("rateprofile %d\r\n", getCurrentControlRateProfile());
        return;
    } else {
        i = atoi(cmdline);
        if (i >= 0 && i < MAX_CONTROL_RATE_PROFILE_COUNT) {
            changeControlRateProfile(i);
            cliRateProfile("");
        }
    }
}

static void cliReboot(void) {
    cliPrint("\r\nRebooting");
    bufWriterFlush(cliWriter);
    waitForSerialPortToFinishTransmitting(cliPort);
    stopMotors();
    handleOneshotFeatureChangeOnRestart();
    systemReset();
}

static void cliSave(char *cmdline)
{
    UNUSED(cmdline);

    cliPrint("Saving");
    writeEEPROM();
    cliReboot();
}

static void cliDefaults(char *cmdline)
{
    UNUSED(cmdline);

    cliPrint("Resetting to defaults");
    resetEEPROM();
    cliReboot();
}

static void cliPrint(const char *str)
{
    while (*str)
        bufWriterAppend(cliWriter, *str++);
}

static void cliPutp(void *p, char ch)
{
    bufWriterAppend(p, ch);
}

static void cliPrintf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    tfp_format(cliWriter, cliPutp, fmt, va);
    va_end(va);
}

static void cliWrite(uint8_t ch)
{
    bufWriterAppend(cliWriter, ch);
}

static void* cliVarPtr(const clivalue_t *var)
{
    const pgRegistry_t* rec = pgFind(var->pgn);

    switch (var->type & VALUE_SECTION_MASK) {
        case CONTROL_RATE_VALUE:
            return rec->address + (sizeof(controlRateConfig_t) * getCurrentControlRateProfile()) + var->offset;
        case PROFILE_VALUE:
            return *rec->ptr + var->offset;
        case MIGRATED_MASTER_VALUE:
            return rec->address + var->offset;
    }
    return NULL;
}

static void cliPrintVar(const clivalue_t *var, uint32_t full)
{
    int32_t value = 0;
    char ftoaBuffer[FTOA_BUFFER_SIZE];

    void *ptr = cliVarPtr(var);

    if(!ptr) {
        cliPrintf("*invalid var type*");
        return;
    }

    switch (var->type & VALUE_TYPE_MASK) {
        case VAR_UINT8:
            value = *(uint8_t*)ptr;
            break;

        case VAR_INT8:
            value = *(int8_t *)ptr;
            break;

        case VAR_UINT16:
            value = *(uint16_t *)ptr;
            break;

        case VAR_INT16:
            value = *(int16_t *)ptr;
            break;

        case VAR_UINT32:
            value = *(uint32_t *)ptr;
            break;

        case VAR_FLOAT:
            if (full && (var->type & VALUE_MODE_MASK) == MODE_DIRECT) {
                cliPrintf("%s", ftoa((float)var->config.minmax.min, ftoaBuffer));
                cliPrintf(" - %s", ftoa((float)var->config.minmax.max, ftoaBuffer));
            } else {
                cliPrintf("%s", ftoa(*(float *)ptr, ftoaBuffer));
            }
            return; // return from case for float only
    }

    switch(var->type & VALUE_MODE_MASK) {
        case MODE_DIRECT:
            if (full) {
                cliPrintf("%d - %d", var->config.minmax.min, var->config.minmax.max);
            } else {
                cliPrintf("%d", value);
            }
            break;
        case MODE_LOOKUP:
            if (full) {
                for (int i=0; i<lookupTables[var->config.lookup.tableIndex].valueCount; i++) {
                    cliPrintf("%s ", lookupTables[var->config.lookup.tableIndex].values[i]);
                }
            } else {
                cliPrintf(lookupTables[var->config.lookup.tableIndex].values[value]);
            }
            break;
    }
}

static void cliSetVar(const clivalue_t *var, const int_float_value_t value)
{
    void *ptr = cliVarPtr(var);

    if(!ptr)
        return;

    switch (var->type & VALUE_TYPE_MASK) {
        case VAR_UINT8:
        case VAR_INT8:
            *(int8_t *)ptr = value.int_value;
            break;

        case VAR_UINT16:
        case VAR_INT16:
            *(int16_t *)ptr = value.int_value;
            break;

        case VAR_UINT32:
            *(uint32_t *)ptr = value.int_value;
            break;

        case VAR_FLOAT:
            *(float *)ptr = (float)value.float_value;
            break;
    }

    if (var->pflags) {
        persistentFlagSet(var->pflags);
    }
}

static void cliSet(char *cmdline)
{
    uint32_t i;
    uint32_t len;
    const clivalue_t *val;
    char *eqptr = NULL;

    len = strlen(cmdline);

    if (len == 0 || (len == 1 && cmdline[0] == '*')) {
        cliPrint("Current settings: \r\n");
        for (i = 0; i < ARRAYLEN(valueTable); i++) {
            val = &valueTable[i];
            cliPrintf("%s = ", valueTable[i].name);
            cliPrintVar(val, len); // when len is 1 (when * is passed as argument), it will print min/max values as well, for gui
            cliPrint("\r\n");
        }
    } else if ((eqptr = strstr(cmdline, "=")) != NULL) {
        // has equals

        char *lastNonSpaceCharacter = eqptr;
        while (*(lastNonSpaceCharacter - 1) == ' ') {
            lastNonSpaceCharacter--;
        }
        uint8_t variableNameLength = lastNonSpaceCharacter - cmdline;

        // skip the '=' and any ' ' characters
        eqptr++;
        while (*(eqptr) == ' ') {
            eqptr++;
        }

        for (i = 0; i < ARRAYLEN(valueTable); i++) {
            val = &valueTable[i];
            // ensure exact match when setting to prevent setting variables with shorter names
            if (strncasecmp(cmdline, valueTable[i].name, strlen(valueTable[i].name)) == 0 && variableNameLength == strlen(valueTable[i].name)) {

                bool changeValue = false;
                int_float_value_t tmp;
                switch (valueTable[i].type & VALUE_MODE_MASK) {
                    case MODE_DIRECT: {
                            if(*eqptr != 0 && strspn(eqptr, "0123456789.+-") == strlen(eqptr)) {
                                int32_t value = 0;
                                float valuef = 0;

                                value = atoi(eqptr);
                                valuef = fastA2F(eqptr);

                                if (valuef >= valueTable[i].config.minmax.min && valuef <= valueTable[i].config.minmax.max) { // note: compare float value

                                    if ((valueTable[i].type & VALUE_TYPE_MASK) == VAR_FLOAT)
                                        tmp.float_value = valuef;
                                    else
                                        tmp.int_value = value;

                                    changeValue = true;
                                }
                            }
                        }
                        break;
                    case MODE_LOOKUP: {
                            const lookupTableEntry_t *tableEntry = &lookupTables[valueTable[i].config.lookup.tableIndex];
                            bool matched = false;
                            for (uint8_t tableValueIndex = 0; tableValueIndex < tableEntry->valueCount && !matched; tableValueIndex++) {
                                matched = strcasecmp(tableEntry->values[tableValueIndex], eqptr) == 0;

                                if (matched) {
                                    tmp.int_value = tableValueIndex;
                                    changeValue = true;
                                }
                            }
                        }
                        break;
                }

                if (changeValue) {
                    cliSetVar(val, tmp);

                    cliPrintf("%s set to ", valueTable[i].name);
                    cliPrintVar(val, 0);
                } else {
                    cliPrint("Invalid value. Allowed values are: ");
                    cliPrintVar(val, 1); // print out min/max/table values
                    cliPrint("\r\n");
                }

                return;
            }
        }
        cliPrint("Invalid name\r\n");
    } else {
        // no equals, check for matching variables.
        cliGet(cmdline);
    }
}

static void cliGet(char *cmdline)
{
    uint32_t i;
    const clivalue_t *val;
    int matchedCommands = 0;

    for (i = 0; i < ARRAYLEN(valueTable); i++) {
        if (strstr(valueTable[i].name, cmdline)) {
            val = &valueTable[i];
            cliPrintf("%s = ", valueTable[i].name);
            cliPrintVar(val, 0);
            cliPrint("\r\n");

            matchedCommands++;
        }
    }


    if (matchedCommands) {
    	return;
    }

    cliPrint("Invalid name\r\n");
}

static void cliStatus(char *cmdline)
{
    UNUSED(cmdline);

    cliPrintf("System Uptime: %d seconds, Voltage: %d * 0.1V (%dS battery - %s), System load: %d.%02d\r\n",
        millis() / 1000, vbat, batteryCellCount, getBatteryStateString(), averageSystemLoadPercent / 100, averageSystemLoadPercent % 100);

    cliPrintf("CPU Clock=%dMHz", (SystemCoreClock / 1000000));

#ifndef CJMCU
    uint8_t i;
    uint32_t mask;
    uint32_t detectedSensorsMask = sensorsMask();

    for (i = 0; ; i++) {

        if (sensorTypeNames[i] == NULL)
            break;

        mask = (1 << i);
        if ((detectedSensorsMask & mask) && (mask & SENSOR_NAMES_MASK)) {
            const char *sensorHardware;
            uint8_t sensorHardwareIndex = detectedSensors[i];
            sensorHardware = sensorHardwareNames[i][sensorHardwareIndex];

            cliPrintf(", %s=%s", sensorTypeNames[i], sensorHardware);

            if (mask == SENSOR_ACC && acc.revisionCode) {
                cliPrintf(".%c", acc.revisionCode);
            }
        }
    }
#endif
    cliPrint("\r\n");

#ifdef USE_I2C
    uint16_t i2cErrorCounter = i2cGetErrorCounter();
#else
    uint16_t i2cErrorCounter = 0;
#endif

    cliPrintf("Cycle Time: %d, I2C Errors: %d, registry size: %d\r\n", cycleTime, i2cErrorCounter, PG_REGISTRY_SIZE);
}

#ifndef SKIP_TASK_STATISTICS
static void cliTasks(char *cmdline)
{
    UNUSED(cmdline);

    cfTaskId_e taskId;
    cfTaskInfo_t taskInfo;

    cliPrintf("Task list          max/us  avg/us rate/hz     total/ms\r\n");
    for (taskId = 0; taskId < TASK_COUNT; taskId++) {
        getTaskInfo(taskId, &taskInfo);
        if (taskInfo.isEnabled) {
            const uint16_t taskFrequency = (uint16_t)(1.0f / ((float)taskInfo.latestDeltaTime * 0.000001f));
            cliPrintf("%2d - %12s, %6d,  %5d,  %5d, %8d\r\n",
                   taskId, taskInfo.taskName, taskInfo.maxExecutionTime, taskInfo.averageExecutionTime, taskFrequency, taskInfo.totalExecutionTime / 1000);
        }
    }
}
#endif

static void cliVersion(char *cmdline)
{
    UNUSED(cmdline);

    cliPrintf("# %s/%s %s %s / %s (%s)",
        FC_NAME,
        targetName,
        FC_VERSION_STRING,
        buildDate,
        buildTime,
        shortGitRevision
    );
}

static void cliPFlags(char *cmdline)
{
    UNUSED(cmdline);

    cliPrintf("# Persistent config flags: 0x%08x", persistentFlags.persistentFlags );
}

void cliProcess(void)
{
    if (!cliWriter) {
        return;
    }

    // Be a little bit tricky.  Flush the last inputs buffer, if any.
    bufWriterFlush(cliWriter);

    while (serialRxBytesWaiting(cliPort)) {
        uint8_t c = serialRead(cliPort);
        if (c == '\t' || c == '?') {
            // do tab completion
            const clicmd_t *cmd, *pstart = NULL, *pend = NULL;
            uint32_t i = bufferIndex;
            for (cmd = cmdTable; cmd < cmdTable + CMD_COUNT; cmd++) {
                if (bufferIndex && (strncasecmp(cliBuffer, cmd->name, bufferIndex) != 0))
                    continue;
                if (!pstart)
                    pstart = cmd;
                pend = cmd;
            }
            if (pstart) {    /* Buffer matches one or more commands */
                for (; ; bufferIndex++) {
                    if (pstart->name[bufferIndex] != pend->name[bufferIndex])
                        break;
                    if (!pstart->name[bufferIndex] && bufferIndex < sizeof(cliBuffer) - 2) {
                        /* Unambiguous -- append a space */
                        cliBuffer[bufferIndex++] = ' ';
                        cliBuffer[bufferIndex] = '\0';
                        break;
                    }
                    cliBuffer[bufferIndex] = pstart->name[bufferIndex];
                }
            }
            if (!bufferIndex || pstart != pend) {
                /* Print list of ambiguous matches */
                cliPrint("\r\033[K");
                for (cmd = pstart; cmd <= pend; cmd++) {
                    cliPrint(cmd->name);
                    cliWrite('\t');
                }
                cliPrompt();
                i = 0;    /* Redraw prompt */
            }
            for (; i < bufferIndex; i++)
                cliWrite(cliBuffer[i]);
        } else if (!bufferIndex && c == 4) {   // CTRL-D
            cliExit(cliBuffer);
            return;
        } else if (c == 12) {                  // NewPage / CTRL-L
            // clear screen
            cliPrint("\033[2J\033[1;1H");
            cliPrompt();
        } else if (bufferIndex && (c == '\n' || c == '\r')) {
            // enter pressed
            cliPrint("\r\n");

            // Strip comment starting with # from line
            char *p = cliBuffer;
            p = strchr(p, '#');
            if (NULL != p) {
                bufferIndex = (uint32_t)(p - cliBuffer);
            }

            // Strip trailing whitespace
            while (bufferIndex > 0 && cliBuffer[bufferIndex - 1] == ' ') {
                bufferIndex--;
            }

            // Process non-empty lines
            if (bufferIndex > 0) {
                cliBuffer[bufferIndex] = 0; // null terminate

                const clicmd_t *cmd;
                for (cmd = cmdTable; cmd < cmdTable + CMD_COUNT; cmd++) {
                    if(!strncasecmp(cliBuffer, cmd->name, strlen(cmd->name))   // command names match
                       && !isalnum((unsigned)cliBuffer[strlen(cmd->name)]))    // next characted in bufffer is not alphanumeric (command is correctly terminated)
                        break;
                }
                if(cmd < cmdTable + CMD_COUNT)
                    cmd->func(cliBuffer + strlen(cmd->name) + 1);
                else
                    cliPrint("Unknown command, try 'help'");
                bufferIndex = 0;
            }

            memset(cliBuffer, 0, sizeof(cliBuffer));

            // 'exit' will reset this flag, so we don't need to print prompt again
            if (!cliMode)
                return;

            cliPrompt();
        } else if (c == 127) {
            // backspace
            if (bufferIndex) {
                cliBuffer[--bufferIndex] = 0;
                cliPrint("\010 \010");
            }
        } else if (bufferIndex < sizeof(cliBuffer) && c >= 32 && c <= 126) {
            if (!bufferIndex && c == ' ')
                continue; // Ignore leading spaces
            cliBuffer[bufferIndex++] = c;
            cliWrite(c);
        }
    }
}

void cliInit()
{
}
#endif

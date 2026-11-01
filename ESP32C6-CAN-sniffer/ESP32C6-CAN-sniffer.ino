#include <Arduino.h>
#include "driver/twai.h"

// Configure TX & RX pins for both SN65HVD230 modules
// Change these according to your hardware setup
static const gpio_num_t CAN0_TX_PIN = GPIO_NUM_14;
static const gpio_num_t CAN0_RX_PIN = GPIO_NUM_20;
static const gpio_num_t CAN1_TX_PIN = GPIO_NUM_1;
static const gpio_num_t CAN1_RX_PIN = GPIO_NUM_3;

// Two global twai handles for CAN0 & CAN1
twai_handle_t twai0_hdl = NULL;
twai_handle_t twai1_hdl = NULL;

// Global variables for CAN bitrates & twai timing configs
// Defaults for HW boot, can be changed later on SavvyCAN settings
twai_timing_config_t timingConfigCAN0 = TWAI_TIMING_CONFIG_500KBITS();
twai_timing_config_t timingConfigCAN1 = TWAI_TIMING_CONFIG_125KBITS();
uint32_t CAN0bitrate = 500000;
uint32_t CAN1bitrate = 125000;

// Global variable for GVRET status options
enum GvretState
{
    GVRET_WAIT_FOR_START,
    GVRET_WAIT_FOR_COMMAND,
    GVRET_READ_PAYLOAD
};

GvretState gvretState = GVRET_WAIT_FOR_START;

// Variables for GVRET binary mode. 
// SavvyCAN requests GVRET binary mode with E7 E7
bool gvretBinaryMode = false;
uint8_t e7Counter = 0;

uint8_t gvretCommand = 0;

uint8_t gvretPayload[32];
size_t gvretPayloadIndex = 0;
size_t gvretPayloadExpected = 0;


// Utility: write uint32 little endian
void writeUint32LE(uint8_t* buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xFF);
    buffer[1] = (uint8_t)((value >> 8) & 0xFF);
    buffer[2] = (uint8_t)((value >> 16) & 0xFF);
    buffer[3] = (uint8_t)((value >> 24) & 0xFF);
}

// Pass the number of actually implemented buses to SavvyCAN
void gvretSendNumBuses()
{
    // No of CAN buses
    // SavvyCAN request: F1 0C
    // Response: F1 0C 02 = device has two CAN buses

    const uint8_t response[] =
    {
        0xF1,
        0x0C,
        0x02
    };

    Serial.write(response, sizeof(response));
}

// Pass the state of both CAN-buses to SavvyCAN
void gvretSendBusInfo()
{
    // Send available CAN bus status
    // SavvyCAN request: F1 06
    // Response: F1 06 11 20 A1 07 00 11 48 E8 01 00
    // => 20 A1 07 00 500kbit for CAN0, 48 E8 01 00 125kbit for CAN1

    uint8_t response[12] = {0};

    response[0] = 0xF1;
    response[1] = 0x06;

    response[2] = 0x11;  // CAN0 enabled + listen-only
    writeUint32LE(&response[3], CAN0bitrate); // CAN0 bitrate

    response[7] = 0x11;  // CAN1 enabled + listen-only
    writeUint32LE(&response[8], CAN1bitrate); // CAN1 bitrate

    Serial.write(response, sizeof(response));
}

// No SWCAN or LIN available. Pass the info to SavvyCAN
void gvretSendExtendedBusInfo()
{
    uint8_t response[17] = {0};

    response[0] = 0xF1;
    response[1] = 0x0D;

    Serial.write(response, sizeof(response));
}

// Pass the device info to SavvyCAN
void gvretSendDeviceInfo()
{
    // Device information
    // SavvyCAN request F1 07

    const uint8_t response[] =
    {
        0xF1,
        0x07,

        0x01,   // firmware/build placeholder
        0x00,
        0x00,
        0x00,
        0x00,
        0x00
    };

    Serial.write(response, sizeof(response));
}

// Validate
void gvretSendValidation()
{
    // Validation information
    // SavvyCAN request F1 09

    const uint8_t response[] =
    {
        0xF1,
        0x09,
        0xDE, // GVRET validate
        0xAD // GVRET validate
    };

    Serial.write(response, sizeof(response));
}

// Timesync
void gvretSendTimeSync()
{
    // SavvyCAN request F1 01

    uint8_t response[6];

    uint32_t timestamp = micros();

    response[0] = 0xF1;
    response[1] = 0x01;

    writeUint32LE(&response[2], timestamp);

    Serial.write(response, sizeof(response));
}

// Send received CAN frame to SavvyCAN
void gvretSendCanFrame(const twai_message_t& frame, uint8_t busNumber)
{
    // Do not stream frames until SavvyCAN has entered GVRET binary mode.
    if (!gvretBinaryMode)
    {
        return;
    }

    uint8_t packet[20];

    uint32_t timestamp = micros();

    uint32_t id = frame.identifier & 0x1FFFFFFF;

    // GVRET uses bit 31 to indicate extended ID.
    if (frame.extd)
    {
        id |= 0x80000000UL;
    }

    // Packet header
    packet[0] = 0xF1;
    packet[1] = 0x00;

    // Timestamp
    writeUint32LE(&packet[2], timestamp);

    // CAN ID
    writeUint32LE(&packet[6], id);

    // Upper 4 bits = bus number, lower 4 bits = Data length code
    // Classic CAN DLC is 0 - 8 bytes. Current HW does not support CAN-FD
    packet[10] =
        ((busNumber & 0x0F) << 4) |
        (frame.data_length_code & 0x0F);

    // CAN payload
    uint8_t length = frame.data_length_code;

    if (length > 8)
    {
        length = 8;
    }

    for (uint8_t i = 0; i < length; i++)
    {
        packet[11 + i] = frame.data[i];
    }

    packet[11 + length] = 0;
    Serial.write(packet, 12 + length);
}

// Convert uint32_t bitrate variable to twai driver timing config variable
twai_timing_config_t convertBitrates(uint32_t bitrate)
{
    switch (bitrate)
    {
        case 1000000:
            return TWAI_TIMING_CONFIG_1MBITS();

        case 800000:
            return TWAI_TIMING_CONFIG_800KBITS();

        case 500000:
            return TWAI_TIMING_CONFIG_500KBITS();

        case 250000:
            return TWAI_TIMING_CONFIG_250KBITS();

        case 125000:
            return TWAI_TIMING_CONFIG_125KBITS();

        case 100000:
            return TWAI_TIMING_CONFIG_100KBITS();

        case 50000:
            return TWAI_TIMING_CONFIG_50KBITS();

        case 25000:
            return TWAI_TIMING_CONFIG_25KBITS();

    }
}

// Update twai driver configs according to SavvyCAN bus bitrate settings
void gvretUpdateBusSettings()
{
    uint32_t can0Config =
        ((uint32_t)gvretPayload[0]) |
        ((uint32_t)gvretPayload[1] << 8) |
        ((uint32_t)gvretPayload[2] << 16) |
        ((uint32_t)gvretPayload[3] << 24);

    uint32_t can1Config =
        ((uint32_t)gvretPayload[4]) |
        ((uint32_t)gvretPayload[5] << 8) |
        ((uint32_t)gvretPayload[6] << 16) |
        ((uint32_t)gvretPayload[7] << 24);

    uint32_t requestedCAN0bitrate = can0Config & 0x0FFFFFFF;
    uint32_t requestedCAN1bitrate = can1Config & 0x0FFFFFFF;

    if (CAN0bitrate == requestedCAN0bitrate &&
        CAN1bitrate == requestedCAN1bitrate)
        {
            return;
        }

    uint32_t previousCAN0bitrate = CAN0bitrate;
    uint32_t previousCAN1bitrate = CAN1bitrate;

    timingConfigCAN0 = convertBitrates(requestedCAN0bitrate);
    timingConfigCAN1 = convertBitrates(requestedCAN1bitrate);
    CAN0bitrate = requestedCAN0bitrate;
    CAN1bitrate = requestedCAN1bitrate;

    if (twai0_hdl != NULL)
    {
        twai_stop_v2(twai0_hdl);
        twai_driver_uninstall_v2(twai0_hdl);
        twai0_hdl = NULL;
    }

    if (twai1_hdl != NULL)
    {
        twai_stop_v2(twai1_hdl);
        twai_driver_uninstall_v2(twai1_hdl);
        twai1_hdl = NULL;
    }

    if (startCan())
    {
        return;
    }

    // Try to restore the previous settings if restarting failed.
    else
    {
        timingConfigCAN0 = convertBitrates(previousCAN0bitrate);
        timingConfigCAN1 = convertBitrates(previousCAN1bitrate);
        CAN0bitrate = previousCAN0bitrate;
        CAN1bitrate = previousCAN1bitrate;
        startCan();
    }
}

// Process GVRET commands
void gvretProcessCommand()
{
    switch (gvretCommand)
    {
        // Time sync
        case 0x01:
            gvretSendTimeSync();
            break;


        // Set CAN parameters
        case 0x05:
            gvretUpdateBusSettings();
            break;


        // Bus information
        case 0x06:
            gvretSendBusInfo();
            break;


        // Device information
        case 0x07:
            gvretSendDeviceInfo();
            break;


        // Validation
        case 0x09:
            gvretSendValidation();
            break;


        // Number of buses
        case 0x0C:
            gvretSendNumBuses();
            break;


        // Extended bus information. No hardware support for SWCAN or LIN
        case 0x0D:
            gvretSendExtendedBusInfo();
            break;


        // Extended bus configuration. 
        // No hardware support for SWCAN or LIN. Ignore command. 
        case 0x0E:
            break;


        // Write message to CAN bus. 
        // Ignored in first version and during testing. Device remains passive (listen only)
        case 0x00:
            break;


        default:
            break;
    }
}

// GVRET byte parser
void gvretHandleByte(uint8_t value)
{
    // SavvyCAN GVRET requests binary mode with E7 E7.

    if (!gvretBinaryMode)
    {
        if (value == 0xE7)
        {
            e7Counter++;

            if (e7Counter >= 2)
            {
                gvretBinaryMode = true;
                e7Counter = 0;

                gvretState = GVRET_WAIT_FOR_START;
            }
        }
        else
        {
            e7Counter = 0;
        }

        return;
    }

    // Parse binary GVRET commands

    switch (gvretState)
    {
        case GVRET_WAIT_FOR_START:
        {
            if (value == 0xF1)
            {
                gvretState = GVRET_WAIT_FOR_COMMAND;
            }

            break;
        }


        case GVRET_WAIT_FOR_COMMAND:
        {
            gvretCommand = value;

            gvretPayloadIndex = 0;
            gvretPayloadExpected = 0;

            // Commands with multiple request (payload) must be collected completely before processing

            if (gvretCommand == 0x00)
            {
                // Reveive and discard an unsupported CAN message transmission request. 
                // CAN message transmission is currently not supported. 
                // However, the whole GVRET packet must be handled to keep GVRET synchronized

                gvretPayloadExpected = 6;
                gvretState = GVRET_READ_PAYLOAD;
            }
            else if (gvretCommand == 0x05)
            {
                // Set CAN parameters.
                gvretPayloadExpected = 9;
                gvretState = GVRET_READ_PAYLOAD;
            }
            else if (gvretCommand == 0x0E)
            {
                // Extended bus configuration.
                gvretPayloadExpected = 13;
                gvretState = GVRET_READ_PAYLOAD;
            }
            else
            {
                gvretProcessCommand();

                gvretState = GVRET_WAIT_FOR_START;
            }

            break;
        }


        case GVRET_READ_PAYLOAD:
        {
            if (gvretPayloadIndex >= sizeof(gvretPayload))
            {
                gvretState = GVRET_WAIT_FOR_START;
                break;
            }

            gvretPayload[gvretPayloadIndex++] = value;

             // CAN TX packet length depends on DLC.
             // After the first six bytes we know how much data follows.

            if (gvretCommand == 0x00 &&
                gvretPayloadIndex == 6)
            {
                uint8_t dlc = gvretPayload[5];

                if (dlc > 8)
                {
                    dlc = 8;
                }

                gvretPayloadExpected =
                    6 + dlc + 1;
            }


            if (gvretPayloadIndex >= gvretPayloadExpected)
            {
                gvretProcessCommand();

                gvretState = GVRET_WAIT_FOR_START;
            }

            break;
        }
    }
}


// USB input
void handleUsbGvret()
{
    while (Serial.available() > 0)
    {
        uint8_t value = (uint8_t)Serial.read();

        gvretHandleByte(value);
    }
}


// CAN initialization, start 2 x TWAI drivers
bool startCan()
{
    twai_general_config_t generalConfigCAN0 =
        TWAI_GENERAL_CONFIG_DEFAULT_V2(
            0,
            CAN0_TX_PIN,
            CAN0_RX_PIN,
            TWAI_MODE_LISTEN_ONLY
        );

    twai_general_config_t generalConfigCAN1 =
        TWAI_GENERAL_CONFIG_DEFAULT_V2(
            1,
            CAN1_TX_PIN,
            CAN1_RX_PIN,
            TWAI_MODE_LISTEN_ONLY
        );
     
    generalConfigCAN0.rx_queue_len = 128;
    generalConfigCAN0.tx_queue_len = 0;

    generalConfigCAN1.rx_queue_len = 128;
    generalConfigCAN1.tx_queue_len = 0;

    twai_filter_config_t filterConfig =
        TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t result =
        twai_driver_install_v2(
            &generalConfigCAN0,
            &timingConfigCAN0,
            &filterConfig,
            &twai0_hdl
        );

    if (result != ESP_OK)
    {
        return false;
    }

    result =
        twai_driver_install_v2(
            &generalConfigCAN1,
            &timingConfigCAN1,
            &filterConfig,
            &twai1_hdl
        );

    if (result != ESP_OK)
    {
        twai_driver_uninstall_v2(twai0_hdl);
        twai0_hdl = NULL;
        twai1_hdl = NULL;
        return false;
    }


    result = twai_start_v2(twai0_hdl);

    if (result != ESP_OK)
    {
        twai_driver_uninstall_v2(twai0_hdl);
        twai_driver_uninstall_v2(twai1_hdl);

        twai0_hdl = NULL;
        twai1_hdl = NULL;

        return false;
    }

    result = twai_start_v2(twai1_hdl);

    if (result != ESP_OK)
    {
        twai_stop_v2(twai0_hdl);
        
        twai_driver_uninstall_v2(twai0_hdl);
        twai_driver_uninstall_v2(twai1_hdl);
        
        twai0_hdl = NULL;
        twai1_hdl = NULL;

        return false;
    }

    return true;
}


// Setup
void setup()
{
    // USB CDC must be enabled in:
    // Tools -> USB CDC On Boot -> Enabled

    Serial.begin(1000000);

    delay(500);

    startCan();
}

// Main loop
void loop()
{
    bool frameReceived = false;
    // Process SavvyCAN commands first.
    handleUsbGvret();

    // Drain all currently waiting CAN frames.
    twai_message_t frame;

    do
    {
        frameReceived = false;

        if (twai_receive_v2(twai0_hdl, &frame, 0) == ESP_OK)
        {
            gvretSendCanFrame(frame, 0);
            frameReceived = true;
        }

        if (twai_receive_v2(twai1_hdl, &frame, 0) == ESP_OK)
        {
            gvretSendCanFrame(frame, 1);
            frameReceived = true;
        }
    }

    while (frameReceived);

}

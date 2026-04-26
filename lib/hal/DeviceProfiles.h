#pragma once

#include <cstdint>
#include <string>

/**
 * HID Device Profiles for Page Turner Devices
 * 
 * Supports multiple page turner devices with their specific keycodes.
 * Each device profile maps button keycodes to page navigation actions.
 */

namespace DeviceProfiles {

// Standard HID Consumer Page codes (preferred, auto-detected first)
constexpr uint8_t STANDARD_PAGE_UP = 0xE9;      // Consumer Page: Page Up
constexpr uint8_t STANDARD_PAGE_DOWN = 0xEA;    // Consumer Page: Page Down

struct DeviceProfile {
  const char* name;           // Device name for display
  const char* macPrefix;      // MAC address prefix to identify device (or nullptr)
  uint8_t pageUpCode;         // HID keycode for page up/previous
  uint8_t pageDownCode;       // HID keycode for page down/next
  bool isConsumerPage;        // If true, these are Consumer Page codes (0xE9, 0xEA range)
  uint8_t reportByteIndex;    // Which byte in HID report contains the keycode
};

// Known device profiles (database of popular page turners)
constexpr DeviceProfile KNOWN_DEVICES[] = {
    // // IINE Game Brick - specific keycodes in byte[4]
    // // Note: MAC prefix set to nullptr to allow ANY GameBrick device
    // {"IINE Game Brick", nullptr, 0x09, 0x07, false, 4},
    
    // // MINI_KEYBOARD - standard keyboard page codes in byte[2]
    // // Note: MAC prefix set to nullptr to allow ANY compatible keyboard
    // {"MINI_KEYBOARD", nullptr, 0x4B, 0x4E, false, 2},
    
    // // Kobo Elipsa 2E remote (common page turner) - Consumer Page
    // {"Kobo Remote", nullptr, STANDARD_PAGE_UP, STANDARD_PAGE_DOWN, true, 2},
    
    // Generic Free2-style device pattern - standard HID Consumer Page
    {"suibian", nullptr, STANDARD_PAGE_UP, STANDARD_PAGE_DOWN, true, 2},
};

constexpr int KNOWN_DEVICES_COUNT = sizeof(KNOWN_DEVICES) / sizeof(KNOWN_DEVICES[0]);

/**
 * Find a device profile by MAC address or device name
 * Returns nullptr if not found in known devices
 */
const DeviceProfile* findDeviceProfile(const char* macAddress, const char* deviceName);

/**
 * Check if keycodes match standard HID Consumer Page codes
 * (0xE9 = Page Up, 0xEA = Page Down)
 */
bool isStandardConsumerPageCode(uint8_t code);

/**
 * Get a user-configured device profile from settings
 * Returns nullptr if no custom profile is set
 */
const DeviceProfile* getCustomProfile();

/**
 * Set a user-configured device profile in settings
 */
void setCustomProfile(uint8_t pageUpCode, uint8_t pageDownCode, uint8_t reportByteIndex);

/**
 * Clear custom profile and revert to auto-detection
 */
void clearCustomProfile();

}  // namespace DeviceProfiles

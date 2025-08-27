# VoodooPS2Elan - Enhanced ELAN Trackpad Driver for macOS

A specialized VoodooPS2 kernel extension that provides comprehensive support for ELAN touchpads on older laptops, delivering a native macOS experience with proper multi-touch gestures, edge detection, and clickpad functionality.

## 🎯 Purpose

This driver was created to solve the persistent issues with ELAN trackpads on older laptops running macOS, particularly the ETD0108 chipset. The standard ApplePS2SmartTouchpad driver proved unreliable and buggy, failing to provide basic functionality like edge detection for macOS gestures and proper button area mapping.

## 📖 Background Story

This project was born out of necessity - developed over 4 weeks to restore proper trackpad functionality on my mother's laptop. The original ApplePS2SmartTouchpad was causing constant issues with basic trackpad operations, making the laptop nearly unusable for everyday tasks. The goal was to create a working VoodooPS2 version that would enable a true Mac experience even for older ELAN trackpads that are often overlooked by modern drivers.

## ✨ Key Features

### 🎯 ETD0108 Chipset Support
- **Multi-touch Edge Detection**: Proper edge swipe recognition for macOS Notification Center and other system gestures
- **Hardware-to-Logical Coordinate Mapping**: Seamless translation between trackpad hardware coordinates and macOS logical coordinates
- **Y-Coordinate Inversion**: Correct coordinate orientation for natural gesture recognition
- **Firmware Support**: Optimized for ETD0108 firmware version 0x381f17

### 🖱️ Advanced Clickpad Functionality
- **Three-Zone Button Areas**: Intelligent left, right, and middle-click detection based on finger position
- **Optimized Button Boundaries**: Live-tested coordinate mapping for accurate click recognition
- **Force Touch Simulation**: Middle-click area provides Force Touch events for Quick Look and other macOS features
- **Realistic Two-Finger Right-Click**: Natural secondary click simulation

### 🎮 Gesture Support
- **Edge Swipes**: Right-edge swipes for Notification Center
- **Multi-finger Gestures**: Support for various macOS trackpad gestures
- **Smooth Scrolling**: Natural scrolling experience with proper acceleration

### 🔧 Smart Coordination Prevention
- **Y-Inversion Stability**: Prevents cursor jumping and erratic movement
- **Edge Detection Workaround**: Reliable edge recognition without coordinate corruption
- **Input System Stability**: Prevents keyboard/trackpad freezing issues

## 🛠️ Technical Improvements

### Coordinate System Fixes
- **Hardware Edge Mapping**: Maps X > 3000 to logical edge coordinates for gesture recognition
- **Boundary Optimization**: Y < 3800 for middle-click area (optimized from live testing)
- **Perfect Button Areas**: X < 1609 for left-click, X ≥ 1609 for right-click

### Multi-touch Enhancements
- **Real-time Finger Tracking**: Accurate multi-finger detection and tracking
- **Pressure Sensitivity**: Proper pressure reporting for Force Touch events
- **Touch Area Calculations**: Realistic finger width and contact area simulation

### Stability Improvements
- **Kernel Extension Safety**: Comprehensive error handling and memory management
- **Input Validation**: Robust coordinate and event validation
- **System Integration**: Seamless interaction with VoodooInput framework

## 📋 Compatibility

### Supported Hardware
- **Primary**: ELAN ETD0108 Touchpad (Firmware 0x381f17)
- **Architecture**: x86_64 Intel-based Macs
- **Bootloader**: OpenCore (recommended)

### Important Technical Note
**ETD0108 Packet Format**: This specific ELAN version uses a different packet format compared to standard v4 protocol. The driver includes a mandatory patch (already active in source code) to operate the trackpad in absolute mode instead of relative mode. This patch is essential for proper multi-touch functionality and coordinate reporting.

### System Requirements
- macOS 10.10+ (Yosemite and later)
- VoodooInput.kext (plugin version recommended)
- Compatible PS/2 controller

## 🚀 Installation

### Prerequisites
1. OpenCore bootloader
2. Disable ApplePS2SmartTouchpad and other conflicting trackpad drivers
3. VoodooInput.kext properly installed

### Installation Steps
1. Copy `VoodooPS2Controller.kext` to your `/EFI/OC/Kexts/` directory
2. Add the kext to your `config.plist` with proper loading order:
   - VoodooPS2Controller.kext
   - VoodooInput.kext (as plugin)
   - VoodooPS2Keyboard.kext
   - VoodooPS2Trackpad.kext
3. Restart and test functionality

## 🔧 Configuration

The driver includes optimized defaults based on extensive testing:
- Middle-click boundary: Y < 3800
- Button split: X = 1609 (50/50 left/right)
- Edge detection: X > 3000 triggers logical edge mapping

## ⚠️ Current Status

While this driver provides a **very good, almost error-free experience**, it's important to note that:

- **Not everything is perfect** - Some edge cases and specific gestures may need refinement
- **Continuous improvement** - The driver can and will be improved based on user feedback and testing
- **Hardware variations** - Different ELAN firmware versions may require additional optimizations
- **Beta status** - While stable in daily use, consider this an actively developed solution

The driver has been extensively tested and provides reliable daily-use functionality, but users should expect occasional minor issues that can be addressed through future updates.

## 🐛 Troubleshooting

### Common Issues
- **Keyboard/Trackpad Freeze**: Ensure clean build and proper kext installation
- **Edge Swipes Not Working**: Verify VoodooInput plugin is loaded correctly
- **Wrong Click Areas**: Check coordinate logging in Console for calibration

### Debug Information
Enable debug logging in Console and filter for "ETD0108" to see real-time coordinate and event information.

## 🤝 Contributing

This driver was developed through extensive source code analysis and testing. Contributions are welcome, especially for:
- Additional ELAN chipset support
- Gesture recognition improvements
- Stability enhancements
- Firmware version compatibility

## 📄 License

Based on VoodooPS2Controller by Acidanthera and RehabMan. Licensed under GPL.

## 🙏 Acknowledgments

- **Acidanthera Team**: For the VoodooInput framework and VoodooPS2 foundation
- **RehabMan**: For original VoodooPS2 development and documentation
- **The macOS Hackintosh Community**: For testing and feedback

## 📈 Project Timeline

- **Development Time**: 4 weeks of intensive source code modification and testing
- **Testing Phase**: Live coordinate mapping and boundary optimization
- **Iterations**: Multiple rebuild cycles to achieve stability
- **Goal Achieved**: Fully functional Mac trackpad experience on older hardware

---

*This driver transforms older ELAN trackpads from frustrating hardware into smooth, responsive input devices that feel native to macOS, while acknowledging there's always room for improvement.*
# ETD0180 Trackpad Testing Instructions

## Prerequisites
1. Build the updated VoodooPS2Controller.kext with the ETD0180 fixes
2. Install the updated kext (requires disabling SIP)
3. Reboot the system

## Systematic Movement Testing

### 1. Y-Axis Inversion Testing
**Expected Behavior:** Moving finger up should move cursor up, moving finger down should move cursor down

**Test Steps:**
1. Open Console.app and filter for "ELAN_CALIB" messages
2. Move finger slowly upward on trackpad
3. **EXPECTED:** Cursor moves upward, log shows negative dy values
4. Move finger slowly downward on trackpad  
5. **EXPECTED:** Cursor moves downward, log shows positive dy values

**Log Analysis:**
- Look for `ETD0180_DELTAS dx=X dy=Y` messages
- Verify dy sign matches movement direction after Y-axis inversion

### 2. Button Detection Testing
**Expected Behavior:** All three buttons (L/R/Middle) should be detected correctly

**Test Steps:**
1. Press and hold left button while moving
2. **EXPECTED:** Log shows `buttons=L1,R0,M0 type=MOVEMENT+LEFT`
3. Press and hold right button while moving
4. **EXPECTED:** Log shows `buttons=L0,R1,M0 type=MOVEMENT+RIGHT`
5. Press and hold middle button while moving
6. **EXPECTED:** Log shows `buttons=L0,R0,M1 type=MOVEMENT+MIDDLE`

**Log Analysis:**
- Check `ETD0180_DELTAS` messages for correct button state
- Verify packet type description matches button pressed

### 3. Boundary Handling Testing
**Expected Behavior:** Cursor should not get "stuck" at edges, movement should be smooth

**Test Steps:**
1. Move cursor to screen edges in all directions
2. Continue moving trackpad finger against the edge
3. **EXPECTED:** No "invisible barriers", smooth movement when moving away from edges
4. Check logs for soft boundary handling (no hard clamping)

**Log Analysis:**
- Look for `ETD0180_OUTPUT` messages showing coordinate changes
- Verify coordinates don't get clamped harshly at boundaries

### 4. Packet Structure Analysis
**Expected Behavior:** Raw packet data should show clear patterns

**Test Steps:**
1. Perform various movements and button combinations
2. Analyze `ETD0180_RAW` and `ETD0180_ANALYSIS` log messages
3. **EXPECTED:** Clear correlation between movement and packet[1]/packet[2] values
4. **EXPECTED:** Button states reflected in packet[0] lower bits

**Key Packet Patterns to Verify:**
- `0x08`: Movement only
- `0x09`: Movement + left button
- `0x0A`: Movement + right button
- `0x0C`: Movement + middle button
- `0x28`: Enhanced movement mode

## Performance Testing

### Movement Precision
- Test slow, precise movements
- Test rapid movements  
- Test diagonal movements
- Test circular movements

### Responsiveness
- Measure input lag (should be minimal)
- Test cursor acceleration curves
- Test gesture recognition (if enabled)

## Debugging Failed Tests

### If Y-axis is still inverted:
- Check that `dy = -dy` line is correctly applied
- Verify the integrator applies `integratedY -= (int)(dy * GAIN)`

### If buttons don't work:
- Check packet[0] values in logs
- Verify button bit masks are correct for your specific packets

### If movement is erratic:
- Check packet structure analysis
- May need to adjust GAIN factor or delta extraction method

## Expected Log Output Examples

```
ELAN_CALIB: ETD0180_RAW [0]=0x08 [1]= -2 [2]=  3 [3]=0x00 [4]=0x00 [5]=0x00
ELAN_CALIB: ETD0180_ANALYSIS byte3=0x00 (bin:00000000) byte4=0x00 byte5=0x00  
ELAN_CALIB: ETD0180_DELTAS dx=-2 dy=3 buttons=L0,R0,M0 type=MOVEMENT_ONLY (0x08)
ELAN_CALIB: ETD0180_OUTPUT x=998 y=1003 integrated=(998,1003)
```

## Success Criteria
- [x] Y-axis movement matches finger direction  
- [x] All buttons detected correctly
- [x] No cursor "sticking" at boundaries
- [x] Smooth, responsive tracking
- [x] Packet structure clearly understood
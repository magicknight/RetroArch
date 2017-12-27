/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2014-2017 - Ali Bouhlel
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * This driver handles the Wii U Gamepad.
 *
 * - For wiimote and wiimote attachments, see kpad_driver.c
 * - For HID controllers, see hid_driver.c
 */

#include <wiiu/pad_driver.h>

static bool wpad_init(void *data);
static bool wpad_query_pad(unsigned pad);
static void wpad_destroy(void);
static bool wpad_button(unsigned pad, uint16_t button);
static void wpad_get_buttons(unsigned pad, retro_bits_t *state);
static int16_t wpad_axis(unsigned pad, uint32_t axis);
static void wpad_poll(void);
static const char *wpad_name(unsigned pad);

static void update_button_state(uint64_t *state, uint32_t held_buttons);
static void update_analog_state(int16_t state[3][2],  VPADStatus *vpad);
static void update_touch_state(int16_t state[3][2], uint64_t *buttons, VPADStatus *vpad);
static void get_touch_coordinates(VPADTouchData *point, VPADStatus *vpad,  bool *clamped);
static void get_calibrated_point(VPADTouchData *point, struct video_viewport *viewport, VPADStatus *vpad);
static void apply_clamping(VPADTouchData *point, struct video_viewport *viewport, bool *clamped);
static void check_panic_button(uint32_t held_buttons);

static int16_t scale_touchpad(int16_t from_min, int16_t from_max,
                              int16_t to_min,   int16_t to_max, int16_t value );

static bool     ready        = false;
static uint64_t button_state = 0;
static int16_t  analog_state[3][2];

static bool wpad_init(void *data) {
  pad_functions.connect(PAD_GAMEPAD, &wpad_driver);
  wpad_poll();
  ready = true;

  return true;
}

static bool wpad_query_pad(unsigned pad) {
  return ready && pad == PAD_GAMEPAD;
}

static void wpad_destroy(void) {
  ready = false;
}

static bool wpad_button(unsigned pad, uint16_t button_bit) {
  if(!wpad_query_pad(pad))
    return false;

  return button_state & (UINT64_C(1) << button_bit);
}

static void wpad_get_buttons(unsigned pad, retro_bits_t *state) {
  if(!wpad_query_pad(pad))
    BIT256_CLEAR_ALL_PTR(state);
  else
    BITS_COPY16_PTR(state, button_state);
}

static int16_t wpad_axis(unsigned pad, uint32_t axis) {
  axis_data data;

  if(!wpad_query_pad(pad) || axis == AXIS_NONE)
    return 0;

  pad_functions.read_axis_data(axis, &data);
  return pad_functions.get_axis_value(data.axis, analog_state, data.is_negative);
}

static void wpad_poll(void) {
  VPADStatus vpad;
  VPADReadError error;

  VPADRead(PAD_GAMEPAD, &vpad, 1, &error);

  if(!error) {
    update_button_state(&button_state, vpad.hold);
    update_analog_state(analog_state, &vpad);
    update_touch_state(analog_state, &button_state, &vpad);
    check_panic_button(vpad.hold);
  }
}

static void update_button_state(uint64_t *state, uint32_t held_buttons)
{
  *state = held_buttons & VPAD_MASK_BUTTONS;
}

static void update_analog_state(int16_t state[3][2],  VPADStatus *vpad)
{
  state[RETRO_DEVICE_INDEX_ANALOG_LEFT] [RETRO_DEVICE_ID_ANALOG_X] = WIIU_READ_STICK(vpad->leftStick.x);
  state[RETRO_DEVICE_INDEX_ANALOG_LEFT] [RETRO_DEVICE_ID_ANALOG_Y] = WIIU_READ_STICK(vpad->leftStick.y);
  state[RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_X] = WIIU_READ_STICK(vpad->rightStick.x);
  state[RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_Y] = WIIU_READ_STICK(vpad->rightStick.y);
}

static void update_touch_state(int16_t state[3][2], uint64_t *buttons, VPADStatus *vpad)
{
  VPADTouchData point = {0};
  bool touch_clamped = false;

  if(!vpad->tpNormal.touched || vpad->tpNormal.validity != VPAD_VALID)
  {
    *buttons &= ~VPAD_BUTTON_TOUCH;
    return;
  }

  get_touch_coordinates(&point, vpad, &touch_clamped);

  state[WIIU_DEVICE_INDEX_TOUCHPAD][RETRO_DEVICE_ID_ANALOG_X] = point.x;
  state[WIIU_DEVICE_INDEX_TOUCHPAD][RETRO_DEVICE_ID_ANALOG_Y] = point.y;

  if(!touch_clamped) {
    *buttons |= VPAD_BUTTON_TOUCH;
  } else {
    *buttons &= ~VPAD_BUTTON_TOUCH;
  }
}

static void get_touch_coordinates(VPADTouchData *point, VPADStatus *vpad,  bool *clamped)
{
  struct video_viewport viewport = {0};

  video_driver_get_viewport_info(&viewport);
  get_calibrated_point(point, &viewport, vpad);
  apply_clamping(point, &viewport, clamped);
}

static void get_calibrated_point(VPADTouchData *point, struct video_viewport *viewport, VPADStatus *vpad) {
   VPADTouchData calibrated720p = {0};

   VPADGetTPCalibratedPoint(PAD_GAMEPAD, &calibrated720p, &(vpad->tpNormal));
   point->x = scale_touchpad(12, 1268, 0, viewport->full_width, calibrated720p.x);
   point->y = scale_touchpad(12, 708, 0, viewport->full_height, calibrated720p.y);
}

static void apply_clamping(VPADTouchData *point, struct video_viewport *viewport, bool *clamped) {
  // clamp the x domain to the viewport
  if(point->x < viewport->x) {
    point->x = viewport->x;
    *clamped = true;
  } else if(point->x > (viewport->x + viewport->width)) {
    point->x = viewport->x + viewport->width;
    *clamped = true;
  }
  // clamp the y domain to the viewport
  if(point->y < viewport->y) {
    point->y = viewport->y;
    *clamped = true;
  } else if(point->y > (viewport->y + viewport->height)) {
    point->y =  viewport->y + viewport->height;
    *clamped = true;
  }
}

static void check_panic_button(uint32_t held_buttons) {
  if( (held_buttons & PANIC_BUTTON_MASK) == PANIC_BUTTON_MASK)
  {
    command_event(CMD_EVENT_QUIT, NULL);
  }
}

static int16_t scale_touchpad(int16_t from_min, int16_t from_max,
                              int16_t to_min,   int16_t to_max, int16_t value )
{
  int32_t from_range = from_max - from_min;
  int32_t to_range = to_max - to_min;

  return (((value - from_min) * to_range) / from_range) + to_min;
}

static const char *wpad_name(unsigned pad) {
  return "WiiU Gamepad";
}

input_device_driver_t wpad_driver =
{
  wpad_init,
  wpad_query_pad,
  wpad_destroy,
  wpad_button,
  wpad_get_buttons,
  wpad_axis,
  wpad_poll,
  NULL,
  wpad_name,
  "gamepad",
};

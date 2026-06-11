#ifndef _MSC_VER
#include <stdbool.h>
#include <sched.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include "libretro.h"
#include <string.h>

#include "StellaLIBRETRO.hxx"
#include "Event.hxx"
#include "NTSCFilter.hxx"
#include "PaletteHandler.hxx"
#include "Version.hxx"


static StellaLIBRETRO stella;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static bool libretro_supports_bitmasks;

// libretro UI settings
static int setting_ntsc, setting_pal;
static int setting_stereo;
static int setting_phosphor, setting_console, setting_phosphor_blend;
static int stella_paddle_joypad_sensitivity;
static int stella_paddle_analog_sensitivity;
static int stella_paddle_mouse_sensitivity;
static int stella_paddle_analog_deadzone;
static bool stella_paddle_analog_absolute;
static bool stella_lightgun_crosshair;
static int setting_crop_hoverscan, crop_left;
static int setting_crop_voverscan, crop_top;
static NTSCFilter::Preset setting_filter;
static const char* setting_palette;

static bool system_reset;

static unsigned input_devices[4];
static int32_t input_crosshair[2];
static Controller::Type input_type[2];

void libretro_logger(int log_level, const char *source)
{
  retro_log_level log_mode = RETRO_LOG_INFO;
  size_t size  = strlen(source);
  char *string = (char*)malloc(size + 1);
  char *token;

  if (!string)
     return;

  strcpy(string, source);
  token = strtok(string, "\n");

  switch (log_level)
  {
    case 2: log_mode = RETRO_LOG_DEBUG; break;
    case 0: log_mode = RETRO_LOG_ERROR; break;
  }

  while (token != NULL)
  {
    log_cb(log_mode, "%s\n", token);
    token = strtok(NULL, "\n");
  }

  free(string);
  string = NULL;
}

// TODO input:
// https://github.com/libretro/blueMSX-libretro/blob/master/libretro.c
// https://github.com/libretro/libretro-o2em/blob/master/libretro.c

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uint32_t libretro_read_rom(void* data)
{
  memcpy(data, stella.getROM(), stella.getROMSize());

  return stella.getROMSize();
}

uint32_t libretro_get_rom_size(void)
{
  return stella.getROMSize();
}

#define RETRO_ANALOG_COMMON() \
  bool mouse_l     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT); \
  bool mouse_r     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT); \
  int32_t mouse_x  = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X); \
  int32_t analog_x = input_state_cb(pad, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X); \
  *input_bitmask  |= mouse_l << RETRO_DEVICE_ID_JOYPAD_B; \
  if (stella_paddle_analog_deadzone && abs(analog_x) < stella_paddle_analog_deadzone * 0x7fff / 100) \
    analog_x = 0; \
  if (mouse_r) \
    mouse_x *= 3; \

static void retro_analog_paddle(unsigned pad, int32_t *analog_axis, int32_t *input_bitmask)
{
  RETRO_ANALOG_COMMON();

  if (mouse_x)
    *analog_axis += mouse_x * stella_paddle_mouse_sensitivity;
  else if (!stella_paddle_analog_absolute)
    *analog_axis += analog_x / 50;
  else
    *analog_axis  = analog_x;
  *analog_axis = BSPF::clamp(*analog_axis, -0x7fff, 0x7fff);
}

static void retro_analog_wheel(unsigned pad, int32_t *analog_axis, int32_t *input_bitmask)
{
  RETRO_ANALOG_COMMON();

  if (mouse_x)
    *analog_axis = mouse_x * stella_paddle_mouse_sensitivity * 50;
  else
    *analog_axis = analog_x;

  *analog_axis = BSPF::clamp(*analog_axis, -0x7fff, 0x7fff);
}

static void draw_crosshair(int16_t x, int16_t y, uint16_t color)
{
   int i;
   int size       = 3;
   int width      = stella.getVideoWidthMax();
   int viewport_w = stella.getVideoWidth();
   int viewport_h = stella.getVideoHeight();
   uint8_t zoom   = stella.getVideoZoom();

   /* crosshair center position */
   uint32_t *ptr = (uint32_t *)stella.getVideoBuffer() + (y * width) + x;

   /* default crosshair dimension */
   int x_start = x - size * zoom;
   int x_end   = x + size * zoom;
   int y_start = y - size;
   int y_end   = y + size;

   if (zoom > 1)
     x_end++;

   /* off-screen */
   if (x <= 0 || y <= 0)
      return;

   /* framebuffer limits */
   if (x_start < 0) x_start = 0;
   if (x_end > viewport_w) x_end = viewport_w;
   if (y_start < 0) y_start = 0;
   if (y_end > viewport_h) y_end = viewport_h;

   /* draw crosshair */
   for (i = (x_start - x); i <= (x_end - x); i++)
   {
      ptr[i] = (i & zoom) ? color : 0xffffff;
   }
   for (i = (y_start - y); i <= (y_end - y); i++)
   {
      ptr[i * width] = (i & 1) ? color : 0xffffff;
      if (zoom > 1)
        ptr[(i * width) + 1] = (i & 1) ? color : 0xffffff;
   }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_input()
{
  if(!input_poll_cb) return;
  input_poll_cb();

#define EVENT stella.setInputEvent
  int32_t input_bitmask[4];
#define GET_BITMASK(pad) \
    if (libretro_supports_bitmasks) \
      input_bitmask[(pad)] = input_state_cb((pad), RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK); \
    else \
    { \
        input_bitmask[(pad)] = 0; \
        for (int i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++) \
          input_bitmask[(pad)] |= input_state_cb((pad), RETRO_DEVICE_JOYPAD, 0, i) ? (1 << i) : 0; \
    }
#define MASK_EVENT(evt, pad, id) stella.setInputEvent((evt), (input_bitmask[(pad)] & (1 << id)) ? 1 : 0)

  input_crosshair[0] = input_crosshair[1] = 0;

  int pad = 0;
  GET_BITMASK(pad)
  switch(input_type[0])
  {
    case Controller::Type::Joy2BPlus:
    case Controller::Type::BoosterGrip:
      MASK_EVENT(Event::LeftJoystickFire9, pad, RETRO_DEVICE_ID_JOYPAD_Y);
      [[fallthrough]];
    case Controller::Type::Genesis:
      MASK_EVENT(Event::LeftJoystickFire5, pad, RETRO_DEVICE_ID_JOYPAD_A);
      [[fallthrough]];
    case Controller::Type::Joystick:
      MASK_EVENT(Event::LeftJoystickLeft,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftJoystickRight, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftJoystickUp,    pad, RETRO_DEVICE_ID_JOYPAD_UP);
      MASK_EVENT(Event::LeftJoystickDown,  pad, RETRO_DEVICE_ID_JOYPAD_DOWN);
      MASK_EVENT(Event::LeftJoystickFire,  pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;

    case Controller::Type::Driving:
    {
      int32_t wheel = 0;

      retro_analog_wheel(pad, &wheel, &input_bitmask[pad]);
      EVENT(Event::LeftDrivingAnalog, wheel);
      MASK_EVENT(Event::LeftDrivingCCW,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftDrivingCW,   pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftDrivingFire, pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    case Controller::Type::Paddles:
    {
      static int32_t paddle_a = 0;
      static int32_t paddle_b = 0;

      retro_analog_paddle(pad, &paddle_a, &input_bitmask[pad]);
      EVENT(Event::LeftPaddleAAnalog, paddle_a);
      MASK_EVENT(Event::LeftPaddleAIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftPaddleADecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftPaddleAFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);

      pad++;
      GET_BITMASK(pad)

      retro_analog_paddle(pad, &paddle_b, &input_bitmask[pad]);
      EVENT(Event::LeftPaddleBAnalog, paddle_b);
      MASK_EVENT(Event::LeftPaddleBIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftPaddleBDecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftPaddleBFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    case Controller::Type::Lightgun:
    {
      // scale from -0x8000..0x7fff to image rect
      const Common::Rect& rect = stella.getImageRect();
      const int32_t x = (input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X) + 0x7fff) * rect.w() / 0xffff;
      const int32_t y = (input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y) + 0x7fff) * rect.h() / 0xffff;

      input_crosshair[0] = x > 0 && x < 0x7fff ? x * stella.getVideoWidth() / rect.w() : 0;
      input_crosshair[1] = y > 0 && y < 0x7fff ? y * stella.getVideoHeight() / rect.h() : 0;

      EVENT(Event::MouseAxisXValue, x);
      EVENT(Event::MouseAxisYValue, y);
      EVENT(Event::MouseButtonLeftValue,  input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER));
      EVENT(Event::MouseButtonRightValue, input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER));
      break;
    }

    case Controller::Type::AmigaMouse:
    case Controller::Type::AtariMouse:
    case Controller::Type::TrakBall:
    {
      bool mouse_l     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
      bool mouse_r     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
      int32_t mouse_x  = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
      int32_t mouse_y  = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
      int32_t analog_x = input_state_cb(pad, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
      int32_t analog_y = input_state_cb(pad, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
      float analog_mag = sqrt((analog_x * analog_x) + (analog_y * analog_y));

      if (stella_paddle_analog_deadzone && analog_mag <= stella_paddle_analog_deadzone * 0x7fff / 100)
        analog_x = analog_y = 0;

      mouse_x += analog_x / (80000 / stella_paddle_analog_sensitivity);
      mouse_y += analog_y / (80000 / stella_paddle_analog_sensitivity);

      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT))
        mouse_x -= stella_paddle_joypad_sensitivity;
      else if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT))
        mouse_x += stella_paddle_joypad_sensitivity;

      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_UP))
        mouse_y -= stella_paddle_joypad_sensitivity;
      else if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN))
        mouse_y += stella_paddle_joypad_sensitivity;

      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_B))
        mouse_l = true;
      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_A))
        mouse_r = true;

      EVENT(Event::MouseAxisXMove, mouse_x);
      EVENT(Event::MouseAxisYMove, mouse_y);
      EVENT(Event::MouseButtonLeftValue,  mouse_l);
      EVENT(Event::MouseButtonRightValue, mouse_r);
      break;
    }

    default:
      break;
  }
  pad++;
  GET_BITMASK(pad)

  switch(input_type[1])
  {
    case Controller::Type::Joy2BPlus:
    case Controller::Type::BoosterGrip:
      MASK_EVENT(Event::RightJoystickFire9, pad, RETRO_DEVICE_ID_JOYPAD_Y);
      [[fallthrough]];
    case Controller::Type::Genesis:
      MASK_EVENT(Event::RightJoystickFire5, pad, RETRO_DEVICE_ID_JOYPAD_A);
      [[fallthrough]];
    case Controller::Type::Joystick:
      MASK_EVENT(Event::RightJoystickLeft,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightJoystickRight, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightJoystickUp,    pad, RETRO_DEVICE_ID_JOYPAD_UP);
      MASK_EVENT(Event::RightJoystickDown,  pad, RETRO_DEVICE_ID_JOYPAD_DOWN);
      MASK_EVENT(Event::RightJoystickFire,  pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;

    case Controller::Type::Driving:
    {
      int32_t wheel = 0;

      retro_analog_wheel(pad, &wheel, &input_bitmask[pad]);
      EVENT(Event::RightDrivingAnalog, wheel);
      MASK_EVENT(Event::RightDrivingCCW,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightDrivingCW,   pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightDrivingFire, pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    case Controller::Type::Paddles:
    {
      static int32_t paddle_a = 0;
      static int32_t paddle_b = 0;

      retro_analog_paddle(pad, &paddle_a, &input_bitmask[pad]);
      EVENT(Event::RightPaddleAAnalog, paddle_a);
      MASK_EVENT(Event::RightPaddleAIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightPaddleADecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightPaddleAFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);

      pad++;
      GET_BITMASK(pad)

      retro_analog_paddle(pad, &paddle_b, &input_bitmask[pad]);
      EVENT(Event::RightPaddleBAnalog, paddle_b);
      MASK_EVENT(Event::RightPaddleBIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightPaddleBDecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightPaddleBFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    default:
      break;
  }

  // Notes:
  // - Each event can only be assigned once, in case of conflicts ususally the latest assignment will be active
  // - The follwing events can also be used by analog devices
  MASK_EVENT(Event::ConsoleLeftDiffA,  0, RETRO_DEVICE_ID_JOYPAD_L);
  MASK_EVENT(Event::ConsoleLeftDiffB,  0, RETRO_DEVICE_ID_JOYPAD_L2);
  MASK_EVENT(Event::ConsoleColor,      0, RETRO_DEVICE_ID_JOYPAD_L3);
  MASK_EVENT(Event::ConsoleRightDiffA, 0, RETRO_DEVICE_ID_JOYPAD_R);
  MASK_EVENT(Event::ConsoleRightDiffB, 0, RETRO_DEVICE_ID_JOYPAD_R2);
  MASK_EVENT(Event::ConsoleBlackWhite, 0, RETRO_DEVICE_ID_JOYPAD_R3);
  MASK_EVENT(Event::ConsoleSelect,     0, RETRO_DEVICE_ID_JOYPAD_SELECT);
  MASK_EVENT(Event::ConsoleReset,      0, RETRO_DEVICE_ID_JOYPAD_START);

#undef EVENT
#undef MASK_EVENT
#undef GET_BITMASK
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_geometry()
{
  struct retro_system_av_info av_info;

  retro_get_system_av_info(&av_info);

  environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_system_av()
{
  struct retro_system_av_info av_info;

  retro_get_system_av_info(&av_info);

  environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_variables(bool init = false)
{
  bool geometry_update = false;

  struct retro_variable var;

#define RETRO_GET(x) \
  var.key = x; \
  var.value = NULL; \
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)

  RETRO_GET("stella_filter")
  {
    NTSCFilter::Preset value = NTSCFilter::Preset::OFF;

    if(!strcmp(var.value, "disabled"))            value = NTSCFilter::Preset::OFF;
    else if(!strcmp(var.value, "composite"))      value = NTSCFilter::Preset::COMPOSITE;
    else if(!strcmp(var.value, "s-video"))        value = NTSCFilter::Preset::SVIDEO;
    else if(!strcmp(var.value, "rgb"))            value = NTSCFilter::Preset::RGB;
    else if(!strcmp(var.value, "badly adjusted")) value = NTSCFilter::Preset::BAD;

    if(setting_filter != value)
    {
      stella.setVideoFilter(value);

      geometry_update = true;
      setting_filter = value;
    }
  }

  RETRO_GET("stella_crop_hoverscan")
  {
    setting_crop_hoverscan = !strcmp(var.value, "enabled");

    geometry_update = true;
  }

  RETRO_GET("stella_crop_voverscan")
  {
    setting_crop_voverscan = atoi(var.value);

    geometry_update = true;
  }

  RETRO_GET("stella_ntsc_aspect")
  {
    int value = 0;

    if(!strcmp(var.value, "par")) value = 0;
    else value = atoi(var.value);

    if(setting_ntsc != value)
    {
      stella.setVideoAspectNTSC(value);

      geometry_update = true;
      setting_ntsc = value;
    }
  }

  RETRO_GET("stella_pal_aspect")
  {
    int value = 0;

    if(!strcmp(var.value, "par")) value = 0;
    else value = atoi(var.value);

    if(setting_pal != value)
    {
      stella.setVideoAspectPAL(value);

      setting_pal = value;
      geometry_update = true;
    }
  }

  RETRO_GET("stella_palette")
  {
    if(setting_palette != var.value)
    {
      stella.setVideoPalette(var.value);

      setting_palette = var.value;
    }
  }

  RETRO_GET("stella_console")
  {
    int value = 0;

    if(!strcmp(var.value, "auto")) value = 0;
    else if(!strcmp(var.value, "ntsc")) value = 1;
    else if(!strcmp(var.value, "pal")) value = 2;
    else if(!strcmp(var.value, "secam")) value = 3;
    else if(!strcmp(var.value, "ntsc50")) value = 4;
    else if(!strcmp(var.value, "pal60")) value = 5;
    else if(!strcmp(var.value, "secam60")) value = 6;

    if(setting_console != value)
    {
      stella.setConsoleFormat(value);

      setting_console = value;
      system_reset = true;
    }
  }

  RETRO_GET("stella_stereo")
  {
    int value = 0;

    if(!strcmp(var.value, "auto")) value = 0;
    else if(!strcmp(var.value, "off")) value = 1;
    else if(!strcmp(var.value, "on")) value = 2;

    if(setting_stereo != value)
    {
      stella.setAudioStereo(value);

      setting_stereo = value;
    }
  }

  RETRO_GET("stella_phosphor")
  {
    int value = 0;

    if(!strcmp(var.value, "auto")) value = 0;
    else if(!strcmp(var.value, "off")) value = 1;
    else if(!strcmp(var.value, "on")) value = 2;

    if(setting_phosphor != value)
    {
      stella.setVideoPhosphor(value, setting_phosphor_blend);

      setting_phosphor = value;
    }
  }

  RETRO_GET("stella_phosphor_blend")
  {
    int value = 0;

    value = atoi(var.value);

    if(setting_phosphor_blend != value)
    {
      stella.setVideoPhosphor(setting_phosphor, value);

      setting_phosphor_blend = value;
    }
  }

  RETRO_GET("stella_paddle_joypad_sensitivity")
  {
    int value = 0;

    value = atoi(var.value);

    if(stella_paddle_joypad_sensitivity != value)
    {
      if(!init) stella.setPaddleJoypadSensitivity(value);

      stella_paddle_joypad_sensitivity = value;
    }
  }

  RETRO_GET("stella_paddle_analog_sensitivity")
  {
    int value = 0;

    value = atoi(var.value);

    if(stella_paddle_analog_sensitivity != value)
    {
      if(!init) stella.setPaddleAnalogSensitivity(value);

      stella_paddle_analog_sensitivity = value;
    }
  }

  RETRO_GET("stella_paddle_mouse_sensitivity")
  {
    stella_paddle_mouse_sensitivity = atoi(var.value);
  }

  RETRO_GET("stella_paddle_analog_deadzone")
  {
    stella_paddle_analog_deadzone = atoi(var.value);
  }

  RETRO_GET("stella_paddle_analog_absolute")
  {
    stella_paddle_analog_absolute = false;

    if(!strcmp(var.value, "enabled"))
      stella_paddle_analog_absolute = true;
  }

  RETRO_GET("stella_lightgun_crosshair")
  {
    stella_lightgun_crosshair = false;

    if(!strcmp(var.value, "enabled"))
      stella_lightgun_crosshair = true;
  }

  if(!init && !system_reset)
  {
    crop_left = setting_crop_hoverscan ? (stella.getVideoZoom() == 2 ? 32 : 8) : 0;
    crop_top  = setting_crop_voverscan;

    if(geometry_update) update_geometry();
  }

#undef RETRO_GET
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static bool reset_system()
{
  // clean restart
  stella.destroy();

  // apply pre-boot settings first
  update_variables(true);

  // start system
  if(!stella.create(log_cb ? true : false)) return false;

  // get auto-detect controllers
  input_type[0] = stella.getLeftControllerType();
  input_type[1] = stella.getRightControllerType();
  stella.setPaddleJoypadSensitivity(stella_paddle_joypad_sensitivity);
  stella.setPaddleAnalogSensitivity(stella_paddle_analog_sensitivity);

  system_reset = false;

  // reset libretro window, apply post-boot settings
  update_variables(false);

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
unsigned retro_get_region()
{
  return stella.getVideoNTSC() ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_get_system_info(struct retro_system_info *info)
{
  *info = retro_system_info{};  // reset to defaults

  info->library_name = stella.getCoreName();
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
  info->library_version = STELLA_VERSION GIT_VERSION;
  info->valid_extensions = stella.getROMExtensions();
  info->need_fullpath = false;
  info->block_extract = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_get_system_av_info(struct retro_system_av_info *info)
{
  *info = retro_system_av_info{};  // reset to defaults
  unsigned crop_width         = crop_left ? 8 : 0;

  info->timing.fps            = stella.getVideoRate();
  info->timing.sample_rate    = stella.getAudioRate();

  info->geometry.base_width   = stella.getRenderWidth();
  info->geometry.base_height  = stella.getRenderHeight();

  info->geometry.max_width    = stella.getVideoWidthMax();
  info->geometry.max_height   = stella.getVideoHeightMax();

  info->geometry.aspect_ratio = stella.getVideoAspectPar() *
      (float)(160 - crop_width) * 2 / (float)(stella.getVideoHeight() - (crop_top * 2));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_set_controller_port_device(unsigned port, unsigned device)
{
  if(port < 4)
  {
    switch(device)
    {
      case RETRO_DEVICE_NONE:
      case RETRO_DEVICE_JOYPAD:
        input_devices[port] = device;
        break;

      default:
        input_devices[port] = RETRO_DEVICE_JOYPAD;
        break;
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_set_environment(retro_environment_t cb)
{
  environ_cb = cb;

  static struct retro_variable variables[] = {
    // Adding more variables and rearranging them is safe.
    { "stella_console", "Console display; auto|ntsc|pal|secam|ntsc50|pal60|secam60" },
    { "stella_palette", "Palette colors; standard|z26|user|custom" },
    { "stella_filter", "TV effects; disabled|composite|s-video|rgb|badly adjusted" },
    { "stella_crop_hoverscan", "Crop horizontal overscan; disabled|enabled" },
    { "stella_crop_voverscan", "Crop vertical overscan; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24" },
    { "stella_ntsc_aspect", "NTSC aspect %; par|100|101|102|103|104|105|106|107|108|109|110|111|112|113|114|115|116|117|118|119|120|121|122|123|124|125|75|76|77|78|79|80|81|82|83|84|85|86|87|88|89|90|91|92|93|94|95|96|97|98|99" },
    { "stella_pal_aspect", "PAL aspect %; par|100|101|102|103|104|105|106|107|108|109|110|111|112|113|114|115|116|117|118|119|120|121|122|123|124|125|75|76|77|78|79|80|81|82|83|84|85|86|87|88|89|90|91|92|93|94|95|96|97|98|99" },
    { "stella_stereo", "Stereo sound; auto|off|on" },
    { "stella_phosphor", "Phosphor mode; auto|off|on" },
    { "stella_phosphor_blend", "Phosphor blend %; 60|65|70|75|80|85|90|95|100|0|5|10|15|20|25|30|35|40|45|50|55" },
    { "stella_paddle_mouse_sensitivity", "Paddle mouse sensitivity; 20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19" },
    { "stella_paddle_joypad_sensitivity", "Paddle joypad sensitivity; 3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|1|2" },
    { "stella_paddle_analog_sensitivity", "Paddle analog sensitivity; 20|21|22|23|24|25|26|27|28|29|30|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19" },
    { "stella_paddle_analog_deadzone", "Paddle analog deadzone; 15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14" },
    { "stella_paddle_analog_absolute", "Paddle analog absolute; disabled|enabled" },
    { "stella_lightgun_crosshair", "Lightgun crosshair; disabled|enabled" },
    { NULL, NULL },
  };

  environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
  (void)level;
  va_list va;
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

void retro_init()
{
  struct retro_log_callback log;
  unsigned level = 4;

  log_cb = fallback_log;
  if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    log_cb = log.log;

  environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
  libretro_supports_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

static const struct retro_controller_description controllers[] = {
    { "Automatic", RETRO_DEVICE_JOYPAD },
    { "None", RETRO_DEVICE_NONE },
    { NULL, 0 }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_load_game(const struct retro_game_info *info)
{
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

  static const struct retro_controller_info controller_info[] = {
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { NULL, 0 }
  };

  #define RETRO_DESCRIPTOR_BLOCK(_user) \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Trigger" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Booster" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Reset" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Left Difficulty A" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Right Difficulty A" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Left Difficulty B" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Right Difficulty B" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Color" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Black/White" }, \
  { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,   RETRO_DEVICE_ID_ANALOG_X, "Axis" } \

  #define RETRO_DESCRIPTOR_EXTRA_BLOCK(_user) \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" }, \
  { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,   RETRO_DEVICE_ID_ANALOG_X, "Axis" } \

  static struct retro_input_descriptor input_descriptors[] =
  {
    RETRO_DESCRIPTOR_BLOCK(0),
    RETRO_DESCRIPTOR_BLOCK(1),
    RETRO_DESCRIPTOR_EXTRA_BLOCK(2),
    RETRO_DESCRIPTOR_EXTRA_BLOCK(3),
    {0, 0, 0, 0, NULL},
  };
  #undef RETRO_DESCRIPTOR_BLOCK
  #undef RETRO_DESCRIPTOR_EXTRA_BLOCK

  if(!info || info->size > stella.getROMMax()) return false;

  // Send controller infos to libretro
  environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)controller_info);
  // Send controller input descriptions to libretro
  environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)input_descriptors);

  if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
  {
    if(log_cb) log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
    return false;
  }

  stella.setROM(info->path, info->data, info->size);

  /* Remember the ROM path and defer the auto-load to the first frame. */
  autoload_rom_path[0] = '\0';
  if (info && info->path)
  {
     strncpy(autoload_rom_path, info->path, sizeof(autoload_rom_path) - 1);
     autoload_rom_path[sizeof(autoload_rom_path) - 1] = '\0';
  }
  autoload_state_pending = true;

  return reset_system();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_reset()
{
  stella.reset();
}

/* ---- Config-driven auto-load save state (cross-platform) ------------ *
 * On the first frame after a ROM loads, the core reads a config file that
 * sits next to it and is named after it: e.g. "stella_libretro.cfg" beside
 * "stella_libretro.dll" (Windows) or "stella_libretro_android.cfg" beside
 * "stella_libretro_android.so" (Android). If enabled, it loads:
 *
 *     <save_path>/<rom-name-without-extension>.<state_ext>
 *
 * Example config (stella_libretro.cfg):
 *     enabled   = 1
 *     save_path = G:\RetroBat\saves\atari2600\libretro.stella
 *     state_ext = state.auto
 *
 * A line "[autoload] ..." is written both to the RetroArch log and to
 * "autoload.log" beside the core, so problems are easy to diagnose.       */

#ifdef _WIN32
#include <windows.h>
#include <time.h>
#define AUTOLOAD_STRCASECMP _stricmp
#define AUTOLOAD_MAX_PATH   MAX_PATH
#define AUTOLOAD_PATH_SEP   '\\'
#else
extern "C" {
#include <dlfcn.h>
}
#include <time.h>
#include <strings.h>
#define AUTOLOAD_STRCASECMP strcasecmp
#define AUTOLOAD_MAX_PATH   4096
#define AUTOLOAD_PATH_SEP   '/'
#endif

#define AUTOLOAD_MAX_RUNS 25   /* keep only the latest N runs in autoload.log */
#define AUTOLOAD_MAX_PATHS 16  /* max number of save_path entries in the config */

static bool autoload_state_pending      = false;
static bool autoload_hotkey_pending     = false;  /* Num1 pressed → reset + autoload */
static char autoload_dir[AUTOLOAD_MAX_PATH]      = {0};   /* folder the core lives in */
static char autoload_rom_path[AUTOLOAD_MAX_PATH] = {0};   /* full path of the loaded ROM   */
static char autoload_core_name[64]      = {0};   /* this core's file name, no ext */
static char autoload_log_path[AUTOLOAD_MAX_PATH + 32] = {0};  /* resolved log file path */

/* Directory that THIS core (.dll/.so) lives in. */
static void autoload_get_self_dir(char *out, size_t out_size)
{
   out[0] = '\0';
#ifdef _WIN32
   HMODULE hm = NULL;
   char path[AUTOLOAD_MAX_PATH];
   char *slash;
   if (!GetModuleHandleExA(
         GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
         (LPCSTR)&autoload_get_self_dir, &hm))
      return;
   if (!GetModuleFileNameA(hm, path, sizeof(path)))
      return;
   slash = strrchr(path, '\\');
   if (slash) *slash = '\0';
   strncpy(out, path, out_size - 1);
   out[out_size - 1] = '\0';
#else
   Dl_info info;
   if (dladdr((const void *)&autoload_get_self_dir, &info) && info.dli_fname)
   {
      char *slash;
      strncpy(out, info.dli_fname, out_size - 1);
      out[out_size - 1] = '\0';
      slash = strrchr(out, '/');
      if (slash) *slash = '\0';
   }
#endif
}

/* This core's own file name, without directory or extension
 * (e.g. "stella_libretro" or "stella_libretro_android"). */
static void autoload_get_self_name(char *out, size_t out_size)
{
   out[0] = '\0';
#ifdef _WIN32
   HMODULE hm = NULL;
   char path[AUTOLOAD_MAX_PATH];
   char *slash, *dot, *base;
   if (!GetModuleHandleExA(
         GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
         (LPCSTR)&autoload_get_self_name, &hm))
      return;
   if (!GetModuleFileNameA(hm, path, sizeof(path)))
      return;
   slash = strrchr(path, '\\');
   base  = slash ? slash + 1 : path;
   strncpy(out, base, out_size - 1);
   out[out_size - 1] = '\0';
   dot = strrchr(out, '.');
   if (dot) *dot = '\0';
#else
   Dl_info info;
   if (dladdr((const void *)&autoload_get_self_name, &info) && info.dli_fname)
   {
      char *slash, *dot, *base;
      strncpy(out, info.dli_fname, out_size - 1);
      out[out_size - 1] = '\0';
      slash = strrchr(out, '/');
      base  = slash ? slash + 1 : out;
      if (base != out) memmove(out, base, strlen(base) + 1);
      dot = strrchr(out, '.');
      if (dot) *dot = '\0';
   }
#endif
}

/* Build the config file path: <core_dir>/<core_name_without_ext>.cfg */
static void autoload_get_self_cfg(char *out, size_t out_size)
{
   char name[64];
   out[0] = '\0';
   autoload_get_self_name(name, sizeof(name));
   if (name[0] == '\0')
      return;
   if (autoload_dir[0] != '\0')
      snprintf(out, out_size, "%s%c%s.cfg", autoload_dir, AUTOLOAD_PATH_SEP, name);
   else
      snprintf(out, out_size, "%s.cfg", name);
}

/* Resolve and cache the autoload.log path.
 * Priority: 1) beside the core (autoload_dir), 2) current working directory. */
static void autoload_resolve_log_path(void)
{
   if (autoload_log_path[0] != '\0')
      return;   /* already resolved */

   if (autoload_dir[0] != '\0')
   {
      snprintf(autoload_log_path, sizeof(autoload_log_path),
               "%s%cautoload.log", autoload_dir, AUTOLOAD_PATH_SEP);
      /* verify we can actually write here */
      FILE *fp = fopen(autoload_log_path, "a");
      if (fp) { fclose(fp); return; }
      /* can't write beside the core — fall through */
      if (log_cb)
         log_cb(RETRO_LOG_WARN,
                "[autoload] can't write autoload.log beside core (path: %s), "
                "trying current directory\n", autoload_log_path);
   }

   /* fallback: current working directory */
   snprintf(autoload_log_path, sizeof(autoload_log_path), "autoload.log");
   FILE *fp = fopen(autoload_log_path, "a");
   if (fp)
   {
      fclose(fp);
      if (log_cb)
         log_cb(RETRO_LOG_INFO,
                "[autoload] autoload.log will be written to current directory\n");
   }
   else
   {
      autoload_log_path[0] = '\0';  /* nothing works */
      if (log_cb)
         log_cb(RETRO_LOG_WARN,
                "[autoload] can't write autoload.log anywhere — "
                "check file permissions\n");
   }
}

/* Write a line to autoload.log next to the core AND to the RetroArch log. */
static void autoload_logf(const char *fmt, ...)
{
   char    line[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[autoload] %s\n", line);

   autoload_resolve_log_path();

   if (autoload_log_path[0] != '\0')
   {
      FILE *fp = fopen(autoload_log_path, "a");
      if (fp) { fprintf(fp, "%s\n", line); fclose(fp); }
   }
}

/* Trim autoload.log so it keeps only the most recent `keep_runs` runs.
 * Runs are delimited by lines beginning with "--- autoload run ---". */
static void autoload_log_rotate(int keep_runs)
{
   const char *marker = "--- autoload run ---";
   FILE       *fp;
   long        fsize;
   char       *data, *p, *cut;
   int         count = 0, seen = 0;
   size_t      got;

   autoload_resolve_log_path();
   if (autoload_log_path[0] == '\0')
      return;

   fp = fopen(autoload_log_path, "rb");
   if (!fp)
      return;                       /* no log yet */
   fseek(fp, 0, SEEK_END);
   fsize = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (fsize <= 0) { fclose(fp); return; }
   data = (char*)malloc((size_t)fsize + 1);
   if (!data) { fclose(fp); return; }
   got = fread(data, 1, (size_t)fsize, fp);
   data[got] = '\0';
   fclose(fp);

   p = data;
   while ((p = strstr(p, marker)) != NULL) { count++; p += 1; }

   if (count > keep_runs)
   {
      int drop = count - keep_runs; /* leading runs to remove */
      cut = data;
      p   = data;
      while ((p = strstr(p, marker)) != NULL)
      {
         if (++seen == drop + 1) { cut = p; break; }
         p += 1;
      }
      fp = fopen(autoload_log_path, "wb");
      if (fp) { fwrite(cut, 1, strlen(cut), fp); fclose(fp); }
   }
   free(data);
}

/* ---- tiny "key = value" config parser ------------------------------- */
typedef struct
{
   bool enabled;
   int  num_paths;
   char save_paths[AUTOLOAD_MAX_PATHS][AUTOLOAD_MAX_PATH];
   char state_ext[64];
} autoload_config;

static void autoload_trim(char *s)
{
   size_t len;
   char  *start = s;
   while (*start == ' ' || *start == '\t') start++;
   if (start != s) memmove(s, start, strlen(start) + 1);
   len = strlen(s);
   while (len > 0 && (s[len-1] == '\r' || s[len-1] == '\n' ||
                      s[len-1] == ' '  || s[len-1] == '\t'))
      s[--len] = '\0';
}

static bool autoload_read_config(const char *cfg_path, autoload_config *cfg)
{
   FILE *fp;
   char  line[1024];

   cfg->enabled      = false;
   cfg->num_paths    = 0;
   strcpy(cfg->state_ext, "state.auto");   /* default extension */

   fp = fopen(cfg_path, "r");
   if (!fp)
      return false;

   while (fgets(line, sizeof(line), fp))
   {
      char *eq, *key, *val;
      if (line[0] == '#' || line[0] == ';')
         continue;
      eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq = '\0';
      key = line;
      val = eq + 1;
      autoload_trim(key);
      autoload_trim(val);

      if (AUTOLOAD_STRCASECMP(key, "enabled") == 0)
         cfg->enabled = (AUTOLOAD_STRCASECMP(val, "1")    == 0 ||
                         AUTOLOAD_STRCASECMP(val, "true") == 0 ||
                         AUTOLOAD_STRCASECMP(val, "yes")  == 0 ||
                         AUTOLOAD_STRCASECMP(val, "on")   == 0);
      else if (AUTOLOAD_STRCASECMP(key, "save_path") == 0)
      {
         if (cfg->num_paths < AUTOLOAD_MAX_PATHS && *val)
         {
            strncpy(cfg->save_paths[cfg->num_paths], val, AUTOLOAD_MAX_PATH - 1);
            cfg->save_paths[cfg->num_paths][AUTOLOAD_MAX_PATH - 1] = '\0';
            cfg->num_paths++;
         }
      }
      else if (AUTOLOAD_STRCASECMP(key, "state_ext") == 0)
      {
         if (*val == '.') val++;            /* accept "state" or ".state" */
         if (*val)
         {
            strncpy(cfg->state_ext, val, sizeof(cfg->state_ext) - 1);
            cfg->state_ext[sizeof(cfg->state_ext) - 1] = '\0';
         }
      }
   }
   fclose(fp);
   return true;
}

/* ROM file name, without directory and without extension.
 * Handles archive content where the frontend passes the path as
 * "<archive>.zip#<inner-file>.a26" - we use the inner file name. */
static void autoload_rom_basename(char *out, size_t out_size)
{
   const char *p    = autoload_rom_path;
   const char *hash = strrchr(p, '#');   /* archive separator, if any */
   const char *base, *s1, *s2;
   char *dot;

   if (hash)
      p = hash + 1;                       /* skip "<archive>.zip#" */

   base = p;
   s1   = strrchr(p, '\\');
   s2   = strrchr(p, '/');
   if (s1 && (!s2 || s1 > s2)) base = s1 + 1;
   else if (s2)                base = s2 + 1;

   strncpy(out, base, out_size - 1);
   out[out_size - 1] = '\0';
   dot = strrchr(out, '.');
   if (dot) *dot = '\0';
}

/* RetroArch wraps states in a "RASTATE" container; the real data is in the
 * "MEM " block. Returns true (and points at the MEM block) if it's a
 * container, false to treat the file as a raw state. */
static bool autoload_extract_rastate(const uint8_t *file, size_t file_len,
                                     const uint8_t **out_data, size_t *out_size)
{
   size_t pos;
   if (file_len < 8 || memcmp(file, "RASTATE", 7) != 0)
      return false;
   pos = 8;
   while (pos + 8 <= file_len)
   {
      const uint8_t *p = file + pos;
      uint32_t blocksize =  (uint32_t)p[4]
                         | ((uint32_t)p[5] << 8)
                         | ((uint32_t)p[6] << 16)
                         | ((uint32_t)p[7] << 24);
      pos += 8;
      if (memcmp(p, "MEM ", 4) == 0)
      {
         if (pos + blocksize > file_len) return false;
         *out_data = file + pos;
         *out_size = (size_t)blocksize;
         return true;
      }
      if (memcmp(p, "END ", 4) == 0)
         break;
      pos += blocksize;
   }
   return false;
}

static void autoload_try_load_state(void)
{
   autoload_config cfg;
   char            cfg_path[AUTOLOAD_MAX_PATH];
   char            romname[AUTOLOAD_MAX_PATH];
   char            file[AUTOLOAD_MAX_PATH * 3];
   void           *buf        = NULL;
   long            len        = 0;
   size_t          expected   = retro_serialize_size();
   const uint8_t  *state_data = NULL;
   size_t          state_size = 0;
   size_t          plen;
   FILE           *fp;

   autoload_get_self_dir(autoload_dir, sizeof(autoload_dir));
   /* Even if core directory is unknown, we can still log to the RetroArch log
    * and possibly to a fallback autoload.log (current working directory). */
   autoload_log_rotate(AUTOLOAD_MAX_RUNS - 1);  /* keep 24; this run makes 25 */

   autoload_get_self_name(autoload_core_name, sizeof(autoload_core_name));

   if (autoload_dir[0] == '\0')
   {
      {
         time_t     t  = time(NULL);
         struct tm *lt = localtime(&t);
         char       ts[32];
         if (lt) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);
         else    ts[0] = '\0';
         autoload_logf("--- autoload run --- core=%s  %s", autoload_core_name[0] ? autoload_core_name : "unknown", ts);
      }
      autoload_logf("RESULT        : could not resolve core directory — "
                    "dladdr/GetModuleHandleEx failed");
      return;
   }

   {
      time_t     t  = time(NULL);
      struct tm *lt = localtime(&t);
      char       ts[32];
      if (lt) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);
      else    ts[0] = '\0';
      autoload_logf("--- autoload run --- core=%s  %s", autoload_core_name, ts);
   }
   autoload_logf("core directory : %s", autoload_dir);

   autoload_get_self_cfg(cfg_path, sizeof(cfg_path));
   autoload_logf("config file   : %s", cfg_path);

   if (!autoload_read_config(cfg_path, &cfg))
   {
      autoload_logf("RESULT        : config file not found - nothing loaded");
      return;
   }

   if (!cfg.enabled)
   {
      autoload_logf("RESULT        : disabled in config (enabled=0)");
      return;
   }

   if (cfg.num_paths == 0)
   {
      autoload_logf("RESULT        : no save_path in config");
      return;
   }

   if (autoload_rom_path[0] == '\0')
   {
      autoload_logf("RESULT        : ROM path unknown (frontend gave none)");
      return;
   }

   autoload_rom_basename(romname, sizeof(romname));
   autoload_logf("rom path      : %s", autoload_rom_path);
   autoload_logf("rom name      : %s", romname);
   autoload_logf("state ext     : %s", cfg.state_ext);
   autoload_logf("core expects  : %u bytes", (unsigned)expected);

   /* Try each save_path in order; the first folder that has a matching
    * state file wins. (Useful for multi-system cores that store states
    * in per-system folders.) */
   {
      int  i;
      bool found = false;
      for (i = 0; i < cfg.num_paths; i++)
      {
         char *sp = cfg.save_paths[i];
         while ((plen = strlen(sp)) > 0 &&
                (sp[plen-1] == '\\' || sp[plen-1] == '/'))
            sp[plen-1] = '\0';

         snprintf(file, sizeof(file), "%s%c%s.%s", sp, AUTOLOAD_PATH_SEP, romname, cfg.state_ext);
         autoload_logf("trying        : %s", file);

         fp = fopen(file, "rb");
         if (fp)
         {
            autoload_logf("found in      : %s", sp);
            found = true;
            break;
         }
      }
      if (!found)
      {
         autoload_logf("RESULT        : no matching state file in any save_path");
         return;
      }
   }

   /* Read the file */
   fseek(fp, 0, SEEK_END);
   len = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   autoload_logf("file size     : %d bytes", (int)len);

   if (fp && len > 0)
   {
      buf = malloc((size_t)len);
      if (buf && fread(buf, 1, (size_t)len, fp) == (size_t)len)
      {
         /* Unwrap RetroArch's RASTATE container if present. */
         if (autoload_extract_rastate((const uint8_t*)buf, (size_t)len,
                                      &state_data, &state_size))
            autoload_logf("RASTATE       : container detected, MEM block = %u bytes",
                          (unsigned)state_size);
         else
         {
            state_data = (const uint8_t*)buf;
            state_size = (size_t)len;
            autoload_logf("RASTATE       : not a container, using raw file");
         }

         if (state_size != expected)
            autoload_logf("WARNING       : state is %u bytes but core expects %u "
                          "(wrong ROM, or RetroArch 'Save State Compression' is ON "
                          "- turn it OFF and re-save).",
                          (unsigned)state_size, (unsigned)expected);

         if (retro_unserialize(state_data, state_size))
            autoload_logf("RESULT        : state loaded OK");
         else
            autoload_logf("RESULT        : retro_unserialize() refused the data");
      }
      else
         autoload_logf("RESULT        : file was empty or unreadable");
   }
   else
      autoload_logf("RESULT        : file was empty");

   if (buf) free(buf);
   if (fp)  fclose(fp);
}

/* ---- End Config-driven auto-load save state ---- */

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_run()
{
  bool updated = false;

  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    update_variables();

  if(system_reset)
  {
    reset_system();
    update_system_av();
    return;
  }

  /* ---- Autoload: load save state after ROM init ---- */
  if (autoload_state_pending)
  {
     autoload_state_pending = false;
     autoload_try_load_state();
  }

  /* ---- Hotkey: Numpad 1 → reset game + autoload save state ---- */
  if (autoload_hotkey_pending)
  {
     autoload_hotkey_pending = false;
     autoload_logf("HOTKEY        : Num1 pressed — resetting game + autoload");
     retro_reset();
     autoload_state_pending = true;
     /* autoload_try_load_state() will run on the next frame */
     return;
  }
  {
     static bool num1_was_pressed = false;
     bool num1_is_pressed = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_KP1);
     if (num1_is_pressed && !num1_was_pressed && autoload_rom_path[0] != '\0')
        autoload_hotkey_pending = true;
     num1_was_pressed = num1_is_pressed;
  }

  update_input();
  stella.runFrame();

  if(stella_lightgun_crosshair && input_crosshair[0] && input_crosshair[1])
    draw_crosshair(input_crosshair[0], input_crosshair[1], 0x0000ff);

  if(stella.getVideoResize())
    update_geometry();

  if(stella.getVideoReady())
    video_cb(reinterpret_cast<uInt32*>(stella.getVideoBuffer()) + crop_left + (crop_top * stella.getVideoWidthMax()),
        stella.getVideoWidth() - crop_left,
        stella.getVideoHeight() - crop_top * 2,
        stella.getVideoPitch());

  if(stella.getAudioReady())
    audio_batch_cb(stella.getAudioBuffer(), stella.getAudioSize());
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_unload_game()
{
  stella.destroy();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_deinit()
{
  stella.destroy();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
size_t retro_serialize_size()
{
  int runahead = -1;
  if(environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &runahead))
  {
    // maximum state size possible
    if(runahead & 4)
      return 0x100000;
  }

  return stella.getStateSize();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_serialize(void *data, size_t size)
{
  return stella.saveState(data, size);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_unserialize(const void *data, size_t size)
{
  return stella.loadState(data, size);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void *retro_get_memory_data(unsigned id)
{
  switch (id)
  {
    case RETRO_MEMORY_SYSTEM_RAM:
      return stella.getRAM();

    default:
      return NULL;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
size_t retro_get_memory_size(unsigned id)
{
  switch (id)
  {
    case RETRO_MEMORY_SYSTEM_RAM:
      return stella.getRAMSize();

    default:
      return 0;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_cheat_reset()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

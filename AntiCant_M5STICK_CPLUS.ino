/* 
Anti Cant for M5StickCPlus

Copyright (C) 2023 DaystateRebel

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

daystaterebel@gmail.com 
*/


#include <M5StickCPlus.h>
#include "M5_ENV.h"
#include <OpenFontRender.h>
#include "ttfbin.h"
#include <OneButton.h>
#include <EEPROM.h>
#include <esp_sleep.h>
#include <PNGdec.h>
#include "DSR.h"

QMP6988 qmp6988;

/* Change these to make pointers wider/taller */
int16_t pointer_sizes[] = {25, 35, 45, 60};

OpenFontRender render;

#define EEPROM_SIZE (6)

#define PIN_INPUT 37
OneButton button(PIN_INPUT, true);

#define DISPLAY_FLIP_OFF    0
#define DISPLAY_FLIP_ON     1
#define DISPLAY_FLIP_MAX    1

#define SENSITIVITY_MAX     45

#define INDICATOR_SIZE_MAX  4

#define UNITS_IMPERIAL  0
#define UNITS_METRIC    1
#define UNITS_MAX       1

#define seconds() (millis()/1000)

static bool renderMenu = false;
static bool dirty = false;
static bool render_info = false;
static bool enviii_present = false;

static uint8_t sensitivity = 1;
static uint8_t tunits = UNITS_IMPERIAL;
static uint8_t punits = UNITS_IMPERIAL;
static uint8_t display_flip = 0;
static uint8_t indicator_size = 2;

#define ROLL_MOVING_AVE_LENGTH  32

#define BAT_SPR_WIDTH   64
#define BAT_SPR_HEIGHT  32

static float roll_moving_average[ROLL_MOVING_AVE_LENGTH];
static float roll_moving_average_total = 0;
static int   roll_moving_average_index = 0;

static float cTemp;
static float fTemp;
static float humidity;


typedef  void (* menuItemCallback_t)(uint8_t);
typedef  void (* menuItemGenString_t)(uint8_t, char *);

TFT_eSprite lspr[2] = {TFT_eSprite(&M5.Lcd), TFT_eSprite(&M5.Lcd)};
TFT_eSprite rspr[2] = {TFT_eSprite(&M5.Lcd), TFT_eSprite(&M5.Lcd)};
TFT_eSprite bspr = TFT_eSprite(&M5.Lcd);

#define MAX_IMAGE_WDITH 240

typedef struct menuItem {
    const char * menuString;
    menuItemGenString_t menuItemGenString;
    struct menuItem * nextMenuItem;
    struct menuItem * currentSubMenu;
    struct menuItem * subMenu;
    menuItemCallback_t menuItemCallback;
    uint8_t param;
    menuItemGenString_t menuItemGenCurSelString;
} menuItem_t;

static menuItem_t * menuStack[4];
static int menuStackIndex;
static menuItem_t * pCurrentMenuItem;

PNG png;

void create_pointer_sprites()
{
  int i;

  for(i=0;i<2;i++){
    lspr[i].createSprite(pointer_sizes[indicator_size], pointer_sizes[indicator_size]);
    rspr[i].createSprite(pointer_sizes[indicator_size], pointer_sizes[indicator_size]);

    lspr[i].fillRect(0, 0, pointer_sizes[indicator_size], pointer_sizes[indicator_size], TFT_BLACK);
    rspr[i].fillRect(0, 0, pointer_sizes[indicator_size], pointer_sizes[indicator_size], TFT_BLACK);
    
    lspr[i].fillTriangle( 1,                                (pointer_sizes[indicator_size]/2), 
                          pointer_sizes[indicator_size]-1,  5, 
                          pointer_sizes[indicator_size]-1,  pointer_sizes[indicator_size]-5, 
                          i == 0 ? TFT_GREEN : TFT_RED);

    rspr[i].fillTriangle( pointer_sizes[indicator_size]-1, (pointer_sizes[indicator_size]/2), 
                          1,                                5, 
                          1,                                pointer_sizes[indicator_size]-5, 
                          i == 0 ? TFT_GREEN : TFT_RED);
  }
}

void setup() {
  M5.begin();

  Serial.printf("Starting Open Display (M5 Stick Plus) V1.1... %d EEPROM bytes\n",EEPROM_SIZE);

  button.attachDoubleClick(doubleClick);
  button.attachLongPressStart(longPressStart);
  button.attachClick(singleClick);

/*
 * Read the Settings, if any setting is out of range
 * give it a sensible default value (cope with first use)
 */
  
  EEPROM.begin(EEPROM_SIZE);
  sensitivity = EEPROM.read(0);
  if(sensitivity > SENSITIVITY_MAX) {
    sensitivity = 1;
    EEPROM.write(0, sensitivity);    
  }
  tunits = EEPROM.read(1);
  if(tunits > UNITS_MAX) {
    tunits = UNITS_MAX;
    EEPROM.write(1, tunits);
  }
  indicator_size = EEPROM.read(2);
  if(indicator_size > INDICATOR_SIZE_MAX) {
    indicator_size = 2;
    EEPROM.write(2, display_flip);
  }
  display_flip = EEPROM.read(3);
  if(display_flip > DISPLAY_FLIP_MAX) {
    display_flip = 0;
    EEPROM.write(3, display_flip);
  }
  punits = EEPROM.read(4);
  if(punits > UNITS_MAX) {
    punits = UNITS_MAX;
    EEPROM.write(4, punits);
  }

  EEPROM.commit();

  M5.Lcd.setRotation(display_flip ? 1 : 3);

	render.setSerial(Serial);	  // Need to print render library message
	render.showFreeTypeVersion(); // print FreeType version
	render.showCredit();		  // print FTL credit

	if (render.loadFont(ttfbin, sizeof(ttfbin)))
	{
		Serial.println("Render initialize error");
		return;
	}

	render.setDrawer(M5.Lcd); // Set drawer object

  M5.Lcd.fillScreen(TFT_BLACK);
  
  Wire.begin(0, 26);
  enviii_present = (qmp6988.init() == 1 ? true : false);

  M5.Imu.Init();  
  Serial.println("IMU Ok");

  bspr.createSprite(BAT_SPR_WIDTH, BAT_SPR_HEIGHT);
  create_pointer_sprites();

  int16_t rc = png.openFLASH((uint8_t *)DSR, sizeof(DSR), pngDraw);  
  if(rc == PNG_SUCCESS) {
    M5.Lcd.startWrite();
    rc = png.decode(NULL, 0);
    M5.Lcd.endWrite();
  }
  delay(5000);
  M5.Lcd.fillScreen(TFT_BLACK);

  dirty = true;
  Serial.println("Startup Complete");    
}

void pngDraw(PNGDRAW *pDraw) {
  uint16_t lineBuffer[MAX_IMAGE_WDITH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  M5.Lcd.pushImage(0, pDraw->y, pDraw->iWidth, 1, lineBuffer);
}

void doRenderMenu() {
  char genHeader[256];
  M5.Lcd.fillScreen(TFT_BLACK);

  const char * pHeadertxt;
  if(pCurrentMenuItem->menuItemGenString == NULL) {
    pHeadertxt = pCurrentMenuItem->menuString;
  } else {
      pHeadertxt = genHeader;
      pCurrentMenuItem->menuItemGenString(pCurrentMenuItem->currentSubMenu->param, genHeader);
  }

  render.setFontSize(30);
  Serial.printf("pHeadertxt '%s'\n",pHeadertxt);
  render.cdrawString(pHeadertxt,
              M5.Lcd.width()/2,
              20,
              TFT_WHITE,
              TFT_BLACK,
              Layout::Horizontal);

  const char * psubHeadertxt = pCurrentMenuItem->currentSubMenu->menuString;
  if(psubHeadertxt == NULL) {
      psubHeadertxt = genHeader;
      pCurrentMenuItem->currentSubMenu->menuItemGenString(pCurrentMenuItem->currentSubMenu->param, genHeader);
  }

  render.setFontSize(20);
  Serial.printf("psubHeadertxt '%s'\n",psubHeadertxt);
  render.cdrawString(psubHeadertxt,
              M5.Lcd.width()/2,
              60,
              TFT_WHITE,
              TFT_BLACK,
              Layout::Horizontal);

  const char * pcurSelHeadertxt;
  if(pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString) {
      pcurSelHeadertxt = genHeader;
      pCurrentMenuItem->currentSubMenu->menuItemGenCurSelString(pCurrentMenuItem->currentSubMenu->param, genHeader);

      render.setFontSize(20);
      Serial.printf("pcurSelHeadertxt '%s'\n",pcurSelHeadertxt);
      render.cdrawString(pcurSelHeadertxt,
                  M5.Lcd.width()/2,
                  90,
                  TFT_WHITE,
                  TFT_BLACK,
                  Layout::Horizontal);
    }
}


void draw_display_battery() {
  uint16_t font_color = TFT_YELLOW;
  char temp_str[16];
  float fbat = M5.Axp.GetBatVoltage();

  bspr.fillRect(0, 0, BAT_SPR_WIDTH, BAT_SPR_HEIGHT, TFT_BLACK);

  if(fbat >= 4.00) {
    font_color = TFT_GREEN;    
  }
  if(fbat < 3.80) {
    font_color = TFT_RED;    
  }

  render.setDrawer(bspr);

  sprintf (temp_str, "%.1fV", fbat);
  render.setFontSize(20);
  render.cdrawString(temp_str,
                BAT_SPR_WIDTH/2,
                0,
                font_color,
                TFT_BLACK,
                Layout::Horizontal);

	render.setDrawer(M5.Lcd); // Set drawer object
  bspr.pushSprite((M5.Lcd.width() - BAT_SPR_WIDTH)/2, 0);
}


//pitch = 180 * atan (accelerationX/sqrt(accelerationY*accelerationY + accelerationZ*accelerationZ))/M_PI;
//roll = 180 * atan (accelerationY/sqrt(accelerationX*accelerationX + accelerationZ*accelerationZ))/M_PI;
//yaw = 180 * atan (accelerationZ/sqrt(accelerationX*accelerationX + accelerationZ*accelerationZ))/M_PI;

void calculate_roll(bool override)
{
  int i;
  float accX = 0.0F;
  float accY = 0.0F;
  float accZ = 0.0F;
  float roll;

  M5.IMU.getAccelData(&accX, &accY, &accZ);
  roll = 180 * atan (accY/sqrt(accX*accX + accZ*accZ))/M_PI;
 
  int old_average = (int)(roll_moving_average_total / ROLL_MOVING_AVE_LENGTH);
  if(old_average < -45) {
    old_average = -45;
  } else if(old_average > 45) {
    old_average = 45;
  }

  roll_moving_average_total -= roll_moving_average[roll_moving_average_index];
  roll_moving_average[roll_moving_average_index] = roll;
  roll_moving_average_total += roll;
  roll_moving_average_index += 1;
  roll_moving_average_index %= ROLL_MOVING_AVE_LENGTH; 

  int new_average = (int)(roll_moving_average_total / ROLL_MOVING_AVE_LENGTH);

  if(new_average < -45) {
    new_average = -45;
  } else if(new_average > 45) {
    new_average = 45;
  }

  if((old_average != new_average) || override) {
    if(new_average > -sensitivity && new_average < sensitivity) {
      lspr[0].pushSprite(0, 68 - new_average - (pointer_sizes[indicator_size]/2));
      rspr[0].pushSprite(240-pointer_sizes[indicator_size], 68 + new_average - (pointer_sizes[indicator_size]/2));
    } else {
      lspr[1].pushSprite(0, 68 - new_average - (pointer_sizes[indicator_size]/2));
      rspr[1].pushSprite(240-pointer_sizes[indicator_size], 68 + new_average - (pointer_sizes[indicator_size]/2));
    }
  }
 }

void SHT3X_get() {
    const uint8_t count = 6;
    uint8_t address = 0x44;
    uint8_t data[count];
    cTemp    = 0;
    fTemp    = 0;
    humidity = 0;

    Wire.beginTransmission(address);
    Wire.write(0x2C);
    Wire.write(0x06);
    if (Wire.endTransmission() != 0) {

      return;
    } 

    delay(200);

    Wire.requestFrom(address, count);
    for (int i = 0; i < count; i++) {
        data[i] = Wire.read();
    };

    delay(50);

    if (Wire.available() != 0) {
       return;
    }

    cTemp    = ((((data[0] * 256.0) + data[1]) * 175) / 65535.0) - 45;
    fTemp    = (cTemp * 1.8) + 32;
    humidity = ((((data[3] * 256.0) + data[4]) * 100) / 65535.0);
}
void doRenderInfo()
{
  char temp_str[32];
  M5.Lcd.fillScreen(TFT_BLACK);
  float p = qmp6988.calcPressure();
//  sht30.UpdateData();
//  t = sht30.GetTemperature();
//  h = sht30.GetRelHumidity();
  SHT3X_get();
  
  if(punits == UNITS_IMPERIAL) {
    sprintf (temp_str, "%.2finHg", p / 3386.389);
  } else {
    sprintf (temp_str, "%.1fmbar", p/100);
  }
  render.setFontSize(25);
  render.cdrawString(temp_str,
                M5.Lcd.width()/2,
                35,
                TFT_WHITE,
                TFT_BLACK,
                Layout::Horizontal);
  if(tunits == UNITS_IMPERIAL) {
    sprintf (temp_str, "%.1fF", fTemp);
  } else {
    sprintf (temp_str, "%.1fC", cTemp);
  }
  render.cdrawString(temp_str,
                M5.Lcd.width()/2,
                68,
                TFT_WHITE,
                TFT_BLACK,
                Layout::Horizontal);

  sprintf (temp_str, "RH %.1f%%", humidity);
  render.cdrawString(temp_str,
                M5.Lcd.width()/2,
                100,
                TFT_WHITE,
                TFT_BLACK,
                Layout::Horizontal);
}

void loop() {
  unsigned long now = seconds();  

  button.tick();
  if(dirty) {
    if(renderMenu){
      doRenderMenu();
      draw_display_battery();
    }
    if (render_info) {
      doRenderInfo();        
      draw_display_battery();
    }
    dirty = false;
  }

  if(!renderMenu && !render_info){
    calculate_roll(dirty);
  }
}

/* 
  Menu system
*/
static void sleepCallback(uint8_t param)
{
  M5.Axp.SetLDO2(false);
  esp_deep_sleep_start();
}

static menuItem_t menu_sleep[] = {
  { "Zzzz",  NULL, menu_sleep, NULL, NULL, sleepCallback, 0, NULL},
};

void menuItemGenStringCurSleep(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_sleep[0].menuString);
}

static void displayFlipCallback(uint8_t param)
{
  display_flip = param;
  EEPROM.write(3, display_flip);
  EEPROM.commit();
  M5.Lcd.setRotation(display_flip ? 1 : 3);
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_display_flip[] = {
  { "Off",         NULL, &menu_display_flip[1], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_OFF, NULL},
  { "On",          NULL, &menu_display_flip[0], NULL, NULL, displayFlipCallback, DISPLAY_FLIP_ON, NULL}
};

void menuItemGenStringCurDisplayFlip(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_display_flip[display_flip].menuString);
}

static void sensitivityIncCallback(uint8_t param)
{
  if(sensitivity <= SENSITIVITY_MAX){
    sensitivity += 1;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
  }
}
static void sensitivityDecCallback(uint8_t param)
{ 
  if(sensitivity > 0){
    sensitivity -= 1;
    EEPROM.write(0, sensitivity);
    EEPROM.commit();
  }
}

static menuItem_t menu_sensitivity[] = {
  { "Increase",     NULL, &menu_sensitivity[1], NULL, NULL, sensitivityIncCallback, 0, NULL},
  { "Decrease",     NULL, &menu_sensitivity[0], NULL, NULL, sensitivityDecCallback, 0, NULL}
};

void menuItemGenStringSensitivity(uint8_t, char * buffer)
{
  sprintf(buffer, "%d Degrees", sensitivity);
}

void menuItemGenStringCurSelSensitivity(uint8_t, char * buffer)
{
  sprintf(buffer, "[%d Degrees]", sensitivity);
}

static void indicatorSzCallback(uint8_t param)
{
  indicator_size = param;
  EEPROM.write(2, indicator_size);
  EEPROM.commit();

  for(int i=0;i<2;i++){
    lspr[i].deleteSprite();
    rspr[i].deleteSprite();
  }
  create_pointer_sprites();

  pCurrentMenuItem = menuStack[--menuStackIndex];
}


static menuItem_t menu_indicator_sz[] = {
  { "Small",         NULL, &menu_indicator_sz[1], NULL, NULL, indicatorSzCallback, 0, NULL},
  { "Medium",        NULL, &menu_indicator_sz[2], NULL, NULL, indicatorSzCallback, 1, NULL},
  { "Large",         NULL, &menu_indicator_sz[3], NULL, NULL, indicatorSzCallback, 2, NULL},
  { "Extra Large",   NULL, &menu_indicator_sz[0], NULL, NULL, indicatorSzCallback, 3, NULL}
};

void menuItemGenStringIndicatorSz(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_indicator_sz[indicator_size].menuString);
}


static void punitsCallback(uint8_t param)
{
  punits = param;
  EEPROM.write(4, punits);
  EEPROM.commit();
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t pmenu_units[] = {
  { "inHg",          NULL, &pmenu_units[1], NULL, NULL, punitsCallback, UNITS_IMPERIAL, NULL},
  { "mbar",          NULL, &pmenu_units[0], NULL, NULL, punitsCallback, UNITS_METRIC, NULL}
};

void menuItemGenStringCurSelPunits(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", pmenu_units[punits].menuString);
}


static void unitsCallback(uint8_t param)
{
  tunits = param;
  EEPROM.write(1, tunits);
  EEPROM.commit();
  pCurrentMenuItem = menuStack[--menuStackIndex];
}

static menuItem_t menu_units[] = {
  { "F",          NULL, &menu_units[1], NULL, NULL, unitsCallback, UNITS_IMPERIAL, NULL},
  { "C",          NULL, &menu_units[0], NULL, NULL, unitsCallback, UNITS_METRIC, NULL}
};

void menuItemGenStringCurSelUnits(uint8_t, char * buffer)
{
  sprintf(buffer, "[%s]", menu_units[tunits].menuString);
}

static menuItem_t menu_top_level[] = {
  { "Indicator Size", NULL,                         &menu_top_level[1], menu_indicator_sz,menu_indicator_sz, NULL, 0, menuItemGenStringIndicatorSz},
  { "Dead Zone",      menuItemGenStringSensitivity, &menu_top_level[2], menu_sensitivity, menu_sensitivity,  NULL, 0, menuItemGenStringCurSelSensitivity},
  { "Temperature",    NULL,                         &menu_top_level[3], menu_units,       menu_units,        NULL, 0, menuItemGenStringCurSelUnits},
  { "Pressure",       NULL,                         &menu_top_level[4], pmenu_units,      pmenu_units,       NULL, 0, menuItemGenStringCurSelPunits},
  { "Display Flip",   NULL,                         &menu_top_level[5], menu_display_flip,menu_display_flip, NULL, 0, menuItemGenStringCurDisplayFlip},
  { "Sleep",          NULL,                         &menu_top_level[0], menu_sleep,       menu_sleep,        NULL, 0, menuItemGenStringCurSleep}
  
};

static menuItem_t menu_entry = {  "Settings", NULL, menu_top_level, menu_top_level, NULL, NULL, 0, NULL };

void singleClick()
{
  Serial.println("x1");
  if(renderMenu)
  {
    dirty = true;
    pCurrentMenuItem->currentSubMenu = pCurrentMenuItem->currentSubMenu->nextMenuItem;
  } else {
    if(enviii_present) {
      dirty = true;
      render_info = !render_info;
      if(!render_info) {
        M5.Lcd.fillScreen(TFT_BLACK);        
      }
    }
  }
}

void doubleClick()
{
Serial.println("x2");  
  if(renderMenu)
  {
    dirty = true;

    if(pCurrentMenuItem->currentSubMenu->menuItemCallback != NULL)
    {
      (*pCurrentMenuItem->currentSubMenu->menuItemCallback)(pCurrentMenuItem->currentSubMenu->param);
    } else {
      if(pCurrentMenuItem->currentSubMenu != NULL) {
        menuStack[menuStackIndex++] = pCurrentMenuItem;
        pCurrentMenuItem = pCurrentMenuItem->currentSubMenu;
      }
    }
  }
}

void longPressStart()
{
  Serial.println("LPS");
  if(!render_info) {
    if(renderMenu) {
      if(menuStackIndex == 1) {
        M5.Lcd.fillScreen(TFT_BLACK);
        renderMenu = false;
        dirty = true;      
      } else {
        dirty = true;
        pCurrentMenuItem = menuStack[--menuStackIndex];
      }
    } else  {
      dirty = true;
      renderMenu = true;
      pCurrentMenuItem = &menu_entry;
      menu_entry.currentSubMenu = menu_top_level;
      menuStackIndex = 0;
      menuStack[menuStackIndex++] = pCurrentMenuItem;
    }
  }
}



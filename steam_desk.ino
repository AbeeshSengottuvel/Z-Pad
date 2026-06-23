#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// --- OLED Settings ---
#define i2c_Address 0x3C
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define OLED_RESET -1
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Joystick Pins (ESP32) ---
#define JOY_X_PIN 34
#define JOY_Y_PIN 35
#define JOY_BTN_PIN 32

void setup() {
  Serial.begin(115200);
  
  // Set up the button pin
  pinMode(JOY_BTN_PIN, INPUT_PULLUP);

  delay(250); // Give OLED time to power up

  // Start the display
  if(!display.begin(i2c_Address, true)) {
    Serial.println(F("SH1106 allocation failed"));
    for(;;); // Freeze if screen isn't found
  }
}

void loop() {
  // 1. Read the joystick values (0 to 4095)
  int xValue = analogRead(JOY_X_PIN);
  int yValue = analogRead(JOY_Y_PIN);
  int btnState = digitalRead(JOY_BTN_PIN); 

  // 2. Clear the screen for the new frame
  display.clearDisplay();

  // --- 3. UI: TEXT SECTION (Left Side) ---
  
  // High-contrast Header Bar
  display.fillRect(0, 0, 58, 12, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK, SH110X_WHITE); // Inverted text
  display.setTextSize(1);
  display.setCursor(5, 2);
  display.print("JOYSTICK");

  // Reset text color for the coordinates
  display.setTextColor(SH110X_WHITE, SH110X_BLACK); 
  
  display.setCursor(0, 18);
  display.print("X: "); display.println(xValue);
  display.setCursor(0, 30);
  display.print("Y: "); display.println(yValue);
  
  // Dynamic Button Status Badge
  if (btnState == LOW) {
    // Pressed State: Solid filled box, black text
    display.fillRoundRect(0, 46, 55, 16, 3, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK, SH110X_WHITE);
    display.setCursor(6, 50);
    display.print("PRESSED");
  } else {
    // Idle State: Outlined box, white text
    display.drawRoundRect(0, 46, 55, 16, 3, SH110X_WHITE);
    display.setTextColor(SH110X_WHITE, SH110X_BLACK);
    display.setCursor(15, 50);
    display.print("IDLE");
  }


  // --- 4. UI: RADAR VISUALIZER (Right Side) ---
  
  // Draw a rounded rectangle for the play area
  display.drawRoundRect(64, 0, 64, 64, 4, SH110X_WHITE);
  
  // Draw faint crosshairs inside the box (using dotted-style spacing or thin lines)
  // Horizontal center line
  display.drawLine(64, 32, 127, 32, SH110X_WHITE); 
  // Vertical center line
  display.drawLine(96, 0, 96, 63, SH110X_WHITE); 
  
  // Hollow out the very center intersection so it looks clean
  display.fillCircle(96, 32, 3, SH110X_BLACK);


  // --- 5. UI: THE MOVING DOT ---
  
  // Convert the 0-4095 joystick numbers into screen coordinates
  // Shrink the map area slightly so the dot doesn't hit the border
  int dotX = map(xValue, 0, 4095, 68, 123); 
  int dotY = map(yValue, 0, 4095, 59, 4); 

  // If the button is pressed, draw a cool "target ring" around the dot
  if (btnState == LOW) {
    display.drawCircle(dotX, dotY, 6, SH110X_WHITE);
  }

  // Draw the main joystick dot
  display.fillCircle(dotX, dotY, 3, SH110X_WHITE);


  // 6. Push everything to the OLED
  display.display();

  // Small delay for smooth animation
  delay(20);
}

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <string.h>
#include <stdlib.h>

// ==========================================
// 1. HARDWARE PINS & CONSTANTS
// ==========================================
const int redLED = 15;     // P1 Emergency LED
const int yellowLED = 2;   // P2 Urgent LED
const int greenLED = 5;    // P3 Normal LED
const int buzzerPin = 4;   // Buzzer

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================================
// 2. DATA STRUCTURES & WEEK 2 ENHANCEMENTS
// ==========================================

enum Priority {
  EMERGENCY = 1,  // P1
  URGENT    = 2,  // P2
  NORMAL    = 3   // P3
};

// --- UPGRADE 3: Dynamic Linked List Node ---
struct PatientNode {
  int id;
  enum Priority priority;
  char name[10];             // e.g., "ID:07"
  unsigned long arrivalTime; // UPGRADE 1: Arrival timestamp via millis()
  PatientNode* next;
};

// Global Linked List Head & Dynamic Queue Size
PatientNode* head = NULL;
int queueSize = 0;

// --- UPGRADE 1: Wait-Time & Statistics Metrics ---
unsigned long totalWaitTimeMs = 0;
int totalPatientsServed = 0;

// --- UPGRADE 2: Auto-Escalation Threshold ---
// Patient priority escalates if wait time exceeds threshold (30s default for testing)
const unsigned long ESCALATION_THRESHOLD_MS = 30000;
unsigned long lastDisplayUpdateMs = 0;

// Multi-Step Keypad Input State Machine
enum SystemState {
  STATE_IDLE,
  STATE_ADD_ENTER_ID,
  STATE_ADD_ENTER_PRIORITY,
  STATE_CONFIRM_RESET
};

SystemState currentState = STATE_IDLE;

// Input Buffers
char inputBuffer[5] = "";
int inputIndex = 0;
int tempPatientID = -1;
int viewIndex = 0;

// ==========================================
// 3. UTILITY & AUDIO-VISUAL FUNCTIONS
// ==========================================

void clearLEDs() {
  digitalWrite(redLED, LOW);
  digitalWrite(yellowLED, LOW);
  digitalWrite(greenLED, LOW);
}

void playBuzzer(int frequency, int durationMs) {
  tone(buzzerPin, frequency, durationMs);
  delay(durationMs);
  noTone(buzzerPin);
}

void triggerPriorityAlert(enum Priority priority) {
  if (priority == EMERGENCY) {
    playBuzzer(1200, 200);
    delay(100);
    playBuzzer(1500, 400);
  } else if (priority == URGENT) {
    playBuzzer(900, 300);
  } else {
    playBuzzer(600, 150);
  }
}

void setPriorityLED(enum Priority priority) {
  clearLEDs();
  if (priority == EMERGENCY) {
    digitalWrite(redLED, HIGH);
  } else if (priority == URGENT) {
    digitalWrite(yellowLED, HIGH);
  } else if (priority == NORMAL) {
    digitalWrite(greenLED, HIGH);
  }
}

const char* getPriorityName(enum Priority p) {
  switch (p) {
    case EMERGENCY: return "EMERGENCY";
    case URGENT:    return "URGENT";
    case NORMAL:    return "NORMAL";
    default:        return "UNKNOWN";
  }
}

// Format milliseconds into human-readable duration (e.g., "45s", "2m15s")
void formatWaitTime(unsigned long waitMs, char* buf, size_t bufSize) {
  unsigned long seconds = waitMs / 1000;
  if (seconds < 60) {
    snprintf(buf, bufSize, "%lus", seconds);
  } else {
    unsigned long minutes = seconds / 60;
    unsigned long remSec = seconds % 60;
    snprintf(buf, bufSize, "%lum%lus", minutes, remSec);
  }
}

// ==========================================
// 4. LINKED LIST & PRIORITY QUEUE ALGORITHMS
// ==========================================

// Linear Search for Duplicate ID in Linked List
bool isDuplicateID(int id) {
  PatientNode* curr = head;
  while (curr != NULL) {
    if (curr->id == id) {
      return true;
    }
    curr = curr->next;
  }
  return false;
}

// UPGRADE 3: Dynamic Priority Insertion Sort in Linked List
bool insertPatient(int id, enum Priority priority) {
  PatientNode* newNode = (PatientNode*)malloc(sizeof(PatientNode));
  if (newNode == NULL) {
    return false; // Dynamic Memory Allocation Failure
  }

  newNode->id = id;
  newNode->priority = priority;
  snprintf(newNode->name, sizeof(newNode->name), "ID:%02d", id);
  newNode->arrivalTime = millis(); // UPGRADE 1: Record arrival timestamp
  newNode->next = NULL;

  // Priority order insertion (1=EMERGENCY, 2=URGENT, 3=NORMAL)
  // Preserves FIFO stability for equal priorities
  if (head == NULL || priority < head->priority) {
    newNode->next = head;
    head = newNode;
  } else {
    PatientNode* curr = head;
    while (curr->next != NULL && curr->next->priority <= priority) {
      curr = curr->next;
    }
    newNode->next = curr->next;
    curr->next = newNode;
  }

  queueSize++;
  return true;
}

// UPGRADE 3 & UPGRADE 1: Dequeue & Track Wait-Time
bool serveNextPatient(PatientNode &servedPatient, unsigned long &patientWaitMs) {
  if (head == NULL) {
    return false; // Queue empty
  }

  PatientNode* temp = head;
  servedPatient = *temp; // Copy patient data
  patientWaitMs = millis() - temp->arrivalTime; // Calculate individual wait time

  // Update global wait metrics
  totalWaitTimeMs += patientWaitMs;
  totalPatientsServed++;

  head = head->next;
  free(temp); // Free allocated memory
  queueSize--;

  return true;
}

// UPGRADE 3: Free All Linked List Memory Nodes
void clearQueue() {
  PatientNode* curr = head;
  while (curr != NULL) {
    PatientNode* temp = curr;
    curr = curr->next;
    free(temp);
  }
  head = NULL;
  queueSize = 0;
}

// Get Patient Node by index (for viewing queue with 'C')
PatientNode* getPatientAtIndex(int index) {
  PatientNode* curr = head;
  int i = 0;
  while (curr != NULL) {
    if (i == index) return curr;
    curr = curr->next;
    i++;
  }
  return NULL;
}

// --- UPGRADE 2: Auto-Escalation Engine (P3 Starvation Prevention) ---
void checkAutoEscalation() {
  if (head == NULL) return;

  unsigned long now = millis();
  PatientNode* curr = head;
  PatientNode* prev = NULL;

  while (curr != NULL) {
    PatientNode* nextNode = curr->next;

    // Check if wait duration exceeds threshold and can be escalated
    if (curr->priority > EMERGENCY && (now - curr->arrivalTime) >= ESCALATION_THRESHOLD_MS) {
      enum Priority oldPriority = curr->priority;
      curr->priority = (enum Priority)((int)curr->priority - 1); // Escalate (e.g., P3 -> P2, P2 -> P1)

      Serial.print("AUTO-ESCALATION: Patient ID ");
      Serial.print(curr->id);
      Serial.print(" bumped from P");
      Serial.print((int)oldPriority);
      Serial.print(" to P");
      Serial.println((int)curr->priority);

      // Detach node from current linked list position
      if (prev == NULL) {
        head = curr->next;
      } else {
        prev->next = curr->next;
      }

      // Re-insert node into linked list maintaining priority order
      if (head == NULL || curr->priority < head->priority) {
        curr->next = head;
        head = curr;
      } else {
        PatientNode* insertPos = head;
        while (insertPos->next != NULL && insertPos->next->priority <= curr->priority) {
          insertPos = insertPos->next;
        }
        curr->next = insertPos->next;
        insertPos->next = curr;
      }

      // Alert tone for auto-escalation
      playBuzzer(1000, 100);

      // Restart iteration after structural re-ordering
      curr = head;
      prev = NULL;
      continue;
    }

    prev = curr;
    curr = nextNode;
  }
}

// ==========================================
// 5. LCD DISPLAY HELPERS
// ==========================================

void displayHomeScreen() {
  lcd.clear();
  if (head == NULL) {
    lcd.setCursor(0, 0);
    lcd.print("Triage: EMPTY");
    lcd.setCursor(0, 1);
    if (totalPatientsServed > 0) {
      char avgBuf[10];
      formatWaitTime(totalWaitTimeMs / totalPatientsServed, avgBuf, sizeof(avgBuf));
      lcd.print("Avg Wait: ");
      lcd.print(avgBuf);
    } else {
      lcd.print("Press A to Add");
    }
    clearLEDs();
  } else {
    // Row 0: NEXT:ID:05 P2 45s
    lcd.setCursor(0, 0);
    lcd.print("NEXT:ID:");
    if (head->id < 10) lcd.print("0");
    lcd.print(head->id);
    lcd.print(" P");
    lcd.print((int)head->priority);

    // Format current wait time of top patient
    char waitBuf[8];
    unsigned long currentWait = millis() - head->arrivalTime;
    formatWaitTime(currentWait, waitBuf, sizeof(waitBuf));
    lcd.print(" ");
    lcd.print(waitBuf);

    // Row 1: Wait:3 Avg:12s
    lcd.setCursor(0, 1);
    lcd.print("Wait:");
    lcd.print(queueSize);

    if (totalPatientsServed > 0) {
      char avgBuf[8];
      formatWaitTime(totalWaitTimeMs / totalPatientsServed, avgBuf, sizeof(avgBuf));
      lcd.print(" Avg:");
      lcd.print(avgBuf);
    }

    setPriorityLED(head->priority);
  }
}

void resetInputBuffer() {
  memset(inputBuffer, 0, sizeof(inputBuffer));
  inputIndex = 0;
}

// ==========================================
// 6. SETUP & MAIN LOOP (STATE MACHINE)
// ==========================================

void setup() {
  Serial.begin(115200);

  pinMode(redLED, OUTPUT);
  pinMode(yellowLED, OUTPUT);
  pinMode(greenLED, OUTPUT);
  pinMode(buzzerPin, OUTPUT);

  clearLEDs();

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Patient Queue");
  lcd.setCursor(0, 1);
  lcd.print("Manager v2.0");

  delay(2000);
  displayHomeScreen();
}

void loop() {
  // Continuous Auto-Escalation Check
  checkAutoEscalation();

  // Periodically refresh home screen in IDLE mode to update real-time wait clock (every 1 second)
  if (currentState == STATE_IDLE && head != NULL && (millis() - lastDisplayUpdateMs >= 1000)) {
    displayHomeScreen();
    lastDisplayUpdateMs = millis();
  }

  char key = keypad.getKey();
  if (!key) return; // No key pressed

  // --- GLOBAL RESET CANCEL ON '*' KEY ---
  if (key == '*' && currentState != STATE_IDLE) {
    currentState = STATE_IDLE;
    resetInputBuffer();
    displayHomeScreen();
    return;
  }

  switch (currentState) {

    // ====================================================
    // STATE 0: IDLE MODE (A=Add, B=Serve, C=View, D=Reset)
    // ====================================================
    case STATE_IDLE:
      if (key == 'A') {
        currentState = STATE_ADD_ENTER_ID;
        resetInputBuffer();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Enter ID (2-digit):");
        lcd.setCursor(0, 1);
        lcd.print("ID: _   [#=Next]");
      } 
      else if (key == 'B') {
        PatientNode served;
        unsigned long waitMs = 0;
        if (serveNextPatient(served, waitMs)) {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("SERVED: ID:");
          if (served.id < 10) lcd.print("0");
          lcd.print(served.id);
          lcd.print(" ");
          lcd.print(getPriorityName(served.priority));

          lcd.setCursor(0, 1);
          char waitBuf[8];
          formatWaitTime(waitMs, waitBuf, sizeof(waitBuf));
          lcd.print("Waited: ");
          lcd.print(waitBuf);

          setPriorityLED(served.priority);
          triggerPriorityAlert(served.priority);
          delay(2200);
          displayHomeScreen();
        } else {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("NO PATIENTS!");
          lcd.setCursor(0, 1);
          lcd.print("Queue is Empty");
          playBuzzer(300, 300);
          delay(1500);
          displayHomeScreen();
        }
      } 
      else if (key == 'C') {
        if (head == NULL) {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("QUEUE EMPTY");
          delay(1200);
          displayHomeScreen();
        } else {
          if (viewIndex >= queueSize) viewIndex = 0;
          PatientNode* p = getPatientAtIndex(viewIndex);
          if (p != NULL) {
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Pos ");
            lcd.print(viewIndex + 1);
            lcd.print("/");
            lcd.print(queueSize);
            lcd.print(" ID:");
            if (p->id < 10) lcd.print("0");
            lcd.print(p->id);

            lcd.setCursor(0, 1);
            lcd.print("P");
            lcd.print((int)p->priority);
            lcd.print(" Wait:");
            char waitBuf[8];
            formatWaitTime(millis() - p->arrivalTime, waitBuf, sizeof(waitBuf));
            lcd.print(waitBuf);

            viewIndex = (viewIndex + 1) % queueSize; // Loop index
          }
        }
      } 
      else if (key == 'D') {
        currentState = STATE_CONFIRM_RESET;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Clear Queue?");
        lcd.setCursor(0, 1);
        lcd.print("[#=Yes | *=Cancel]");
      }
      break;

    // ====================================================
    // STATE 1: MULTI-STEP - ENTER 2-DIGIT PATIENT ID
    // ====================================================
    case STATE_ADD_ENTER_ID:
      if (key >= '0' && key <= '9') {
        if (inputIndex < 2) {
          inputBuffer[inputIndex++] = key;
          inputBuffer[inputIndex] = '\0';

          lcd.setCursor(4, 1);
          lcd.print(inputBuffer);
          lcd.print(" ");
        }
      } 
      else if (key == '#') {
        if (inputIndex == 0) {
          lcd.setCursor(0, 0);
          lcd.print("ID CANNOT BE BLANK");
          delay(1000);
          lcd.setCursor(0, 0);
          lcd.print("Enter ID (2-digit):");
        } else {
          int enteredID = atoi(inputBuffer);
          if (isDuplicateID(enteredID)) {
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("DUPLICATE ID!");
            lcd.setCursor(0, 1);
            lcd.print("Try Another ID");
            playBuzzer(400, 500);
            delay(1500);
            resetInputBuffer();
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Enter ID (2-digit):");
            lcd.setCursor(0, 1);
            lcd.print("ID: _   [#=Next]");
          } else {
            tempPatientID = enteredID;
            currentState = STATE_ADD_ENTER_PRIORITY;
            resetInputBuffer();

            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("ID:");
            if (tempPatientID < 10) lcd.print("0");
            lcd.print(tempPatientID);
            lcd.print(" Set Priority:");
            lcd.setCursor(0, 1);
            lcd.print("1:P1 2:P2 3:P3 [#]");
          }
        }
      }
      break;

    // ====================================================
    // STATE 2: MULTI-STEP - ENTER PRIORITY LEVEL (1, 2, 3)
    // ====================================================
    case STATE_ADD_ENTER_PRIORITY:
      if (key >= '1' && key <= '3') {
        inputBuffer[0] = key;
        inputBuffer[1] = '\0';
        inputIndex = 1;

        lcd.setCursor(14, 0);
        lcd.print(key);
      } 
      else if (key == '#') {
        if (inputIndex == 0) {
          lcd.setCursor(0, 0);
          lcd.print("SELECT 1, 2, OR 3!");
          delay(1000);
          lcd.setCursor(0, 0);
          lcd.print("ID:");
          if (tempPatientID < 10) lcd.print("0");
          lcd.print(tempPatientID);
          lcd.print(" Set Priority:");
        } else {
          int prioVal = inputBuffer[0] - '0';
          enum Priority selectedPriority = (enum Priority)prioVal;

          if (!insertPatient(tempPatientID, selectedPriority)) {
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("MEMORY FULL!");
            playBuzzer(400, 500);
            delay(1500);
          } else {
            triggerPriorityAlert(selectedPriority);

            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("Added P");
            lcd.print(prioVal);
            lcd.print(" - ID:");
            if (tempPatientID < 10) lcd.print("0");
            lcd.print(tempPatientID);

            lcd.setCursor(0, 1);
            lcd.print("Queue Total: ");
            lcd.print(queueSize);

            delay(1800);
          }

          // Reset to Idle
          currentState = STATE_IDLE;
          resetInputBuffer();
          displayHomeScreen();
        }
      }
      break;

    // ====================================================
    // STATE 3: CONFIRM CLEAR QUEUE
    // ====================================================
    case STATE_CONFIRM_RESET:
      if (key == '#') {
        clearQueue();
        viewIndex = 0;
        clearLEDs();

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("QUEUE CLEARED!");
        playBuzzer(800, 400);
        delay(1200);

        currentState = STATE_IDLE;
        displayHomeScreen();
      } else {
        // Any key other than '#' cancels
        currentState = STATE_IDLE;
        displayHomeScreen();
      }
      break;
  }
}

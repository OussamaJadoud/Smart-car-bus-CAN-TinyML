#include <SPI.h>
#include <mcp_can.h>

// Broches de commande du pont en H L298N
const int PIN_IN2 = 4;        // Moteur Gauche
const int PIN_ENA = 5;        // PWM Moteur Gauche
const int PIN_ENB = 6;        // PWM Moteur Droit
const int PIN_IN3 = 7;        // Moteur Droit (IN3)
const int PIN_IN4 = 8;        // Moteur Droit (IN4)
const int PIN_IN1 = 9;        // Moteur Gauche
const int CS_CAN  = 10;       // Module CAN MCP2515 Chip Select

MCP_CAN CAN0(CS_CAN);

// Broches des capteurs optiques de vitesse (Encodeurs)
const int PIN_CAPTEUR_G = 2;  
const int PIN_CAPTEUR_D = 3;  
volatile unsigned long ticksGauche = 0;
volatile unsigned long ticksDroit  = 0;

void intGauche() { ticksGauche++; }
void intDroit()  { ticksDroit++; }

unsigned long precedentMillis = 0;
const int DT = 100; // Fenêtre d'échantillonnage de 100ms (10 Hz)

// Paramètres de l'asservissement PI 
float Kp_sync = 2.5;  
float Ki_sync = 0.4;  
float erreurCumuleeSync = 0;

// EQUILIBRAGE ASYMETRIQUE DU CHASSIS
float Kp_moteur_gauche = 1.5;  // Compensation du moteur gauche mécaniquement faible
float Kp_moteur_droit  = 0.75; // Atténuation du moteur droit trop puissant 

// Variables de controle de trame CAN
byte modeActuel = 0; 
byte consigneVitesse = 0; 
byte freinUrgence = 1; 
int8_t consigneAngle = 0;
unsigned long dernierMessageCan = 0;

void setup() {
    Serial.begin(115200);
    
    pinMode(PIN_ENA, OUTPUT); pinMode(PIN_ENB, OUTPUT);
    pinMode(PIN_IN1, OUTPUT); pinMode(PIN_IN2, OUTPUT);
    pinMode(PIN_IN3, OUTPUT); pinMode(PIN_IN4, OUTPUT);

    pinMode(PIN_CAPTEUR_G, INPUT); pinMode(PIN_CAPTEUR_D, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_CAPTEUR_G), intGauche, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_CAPTEUR_D), intDroit,  FALLING);

    // Initialisation du controleur CAN à 500 Kbps (Quartz 8 MHz)
    if (CAN0.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK) {
        CAN0.setMode(MCP_NORMAL);
    } else { 
        while(1); // Bloque le système si le module CAN est débranché
    }
}

void loop() {
    long unsigned int rxId;
    unsigned char len = 0;
    unsigned char rxBuf[8];

    // LECTURE DU BUS CAN (Réception des consignes de la passerelle)
    if (CAN0.checkReceive() == CAN_MSGAVAIL) {
        CAN0.readMsgBuf(&rxId, &len, rxBuf);
        if (rxId == 0x100) {
            modeActuel      = rxBuf[0]; // 0=Manu, 1=Vocal, 2=SD
            consigneVitesse = rxBuf[1];
            freinUrgence    = rxBuf[2];
            consigneAngle   = (int8_t)rxBuf[3]; // Angle signé (-90 à +90)
            dernierMessageCan = millis(); 
        }
    }

    // Sécurité (Watchdog CAN) : Arrêt si déconnexion > 1 seconde
    if (millis() - dernierMessageCan > 1000) { 
        freinUrgence = 1; 
    }

    // TRAITEMENT TOUTES LES 100 ms (Calcul et PI)
    unsigned long actuelMillis = millis();
    if (actuelMillis - precedentMillis >= DT) {
        precedentMillis = actuelMillis;

        // Isolation des variables volatiles pour la lecture
        noInterrupts();
        float vitActuelleG = ticksGauche; 
        float vitActuelleD = ticksDroit;
        ticksGauche = 0; ticksDroit = 0;
        interrupts();

        // Gestion de l'arrêt complet
        if (freinUrgence == 1 || consigneVitesse == 0) {
            analogWrite(PIN_ENA, 0);    analogWrite(PIN_ENB, 0);
            digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, LOW);
            digitalWrite(PIN_IN3, LOW); digitalWrite(PIN_IN4, LOW);
            erreurCumuleeSync = 0;
            
            unsigned char data[1] = { 0 }; 
            CAN0.sendMsgBuf(0x102, 0, 1, data); // Télémétrie Vitesse = 0
            return; 
        }

        float cibleG = consigneVitesse;
        float cibleD = consigneVitesse;
        float ratioVirage = abs(consigneAngle) / 90.0;
        float correctionSync = 0;

        // LOGIQUE DE LIGNE DROITE : Correction PI de trajectoire active
        if (abs(consigneAngle) <= 10) {
            float erreurSync = vitActuelleG - vitActuelleD; 
            erreurCumuleeSync = constrain(erreurCumuleeSync + erreurSync, -50, 50);
            correctionSync = (Kp_sync * erreurSync) + (Ki_sync * erreurCumuleeSync);
            
            cibleG = consigneVitesse - correctionSync;
            cibleD = consigneVitesse + correctionSync;
        } 
        // LOGIQUE DE VIRAGE : Différentiel asymétrique directionnel
        else {
            erreurCumuleeSync = 0; // Reset du terme intégral
            
            if (consigneAngle < -10) { 
                // Virage à Droite (Angle négatif) : Moteur gauche ralentit
                cibleG = consigneVitesse - (consigneVitesse * ratioVirage);
                cibleD = consigneVitesse; 
            } 
            else if (consigneAngle > 10) { 
                // Virage à Gauche (Angle positif) : Moteur droit ralentit
                cibleD = consigneVitesse - (consigneVitesse * ratioVirage);
                cibleG = consigneVitesse + (consigneVitesse * 0.55 * ratioVirage); 
            }
        }

        // Calculation finale des PWM et application des coefficients d'équilibrage
        int pwmG = constrain((int)(cibleG * Kp_moteur_gauche), 0, 255);
        int pwmD = constrain((int)(cibleD * Kp_moteur_droit),  0, 255);

        // Sorties numériques : Configuration du sens de marche (Marche Avant)
        digitalWrite(PIN_IN1, HIGH); digitalWrite(PIN_IN2, LOW);  // Bloc gauche
        digitalWrite(PIN_IN3, LOW);  digitalWrite(PIN_IN4, HIGH); // Bloc droit (Inversion OUT3/OUT4)

        // Génération physique du signal PWM sur les drivers moteurs
        analogWrite(PIN_ENA, pwmG);
        analogWrite(PIN_ENB, pwmD);

        // TÉLÉMÉTRIE : Envoi de la vitesse moyenne convertie en Ticks/seconde
        byte vitesseMoyenne = (vitActuelleG + vitActuelleD) * 5;

        // Monitoring série
        Serial.print("[VOITURE] V: "); Serial.print(vitesseMoyenne);
        Serial.print(" | Ticks G: "); Serial.print(vitActuelleG, 0);
        Serial.print(" | Ticks D: "); Serial.print(vitActuelleD, 0);
        Serial.print(" || PWM_G: "); Serial.print(pwmG);
        Serial.print(" | PWM_D: "); Serial.println(pwmD);

        // Envoi de la trame de feedback à l'ESP32 via le bus CAN (ID 0x102)
        unsigned char data[1] = { vitesseMoyenne }; 
        CAN0.sendMsgBuf(0x102, 0, 1, data);
    }
}
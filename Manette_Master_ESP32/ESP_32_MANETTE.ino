#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <WebServer.h>

// Configuration des Entrées/Sorties Physiques 
#define PIN_BTN_ACCEL  23  // Bouton Incrémentation Vitesse
#define PIN_BTN_DECEL  15  // Bouton Décrémentation Vitesse
#define PIN_BTN_MODE   4   // Bouton Commutation de Mode (Manuel / Vocal / Auto)

// Bus I2C & Réseau
LiquidCrystal_I2C lcd(0x27, 16, 2); // Esclave LCD (Adresse  0x27)
Adafruit_MPU6050 mpu;               // Esclave Centrale Inertielle (Adresse 0x68)
WebServer server(80);               // Serveur HTTP sur le port standard 80

// Adresse MAC cible de l'ESP32 Passerelle (Voiture)
uint8_t adresseVoiture[] = {0x3C, 0x61, 0x05, 0x2F, 0xDA, 0xB8}; 

// Couche Liaison de Données : Structures Réseau 
typedef struct __attribute__((packed)) struct_commande {
    byte mode;    // 0 = Manuel, 1 = Vocal, 2 = SD Autonome
    byte vitesse; // Consigne PWM (0 à 250)
    byte frein;   // Drapeau de sécurité (0 = OK, 1 = Arrêt d'Urgence)
    int8_t angle; // Angle signé de braquage (-90 à +90°)
} struct_commande;

typedef struct __attribute__((packed)) struct_feedback {
    byte vitesse_reelle; //Télémétrie brute en Ticks/seconde renvoyée par la Nano
} struct_feedback;

struct_commande mesCommandes;
struct_feedback retourVoiture;

// Horloges Logicielles 
unsigned long chronoAffichage = 0;
unsigned long chronoVitesse = 0; 

// Interface Homme-Machine (IHM) Vocale  
// Déploiement du Web Speech API native du navigateur pour décharger l'ESP32 de la lourdeur d'un modèle d'inférence TinyML local
const char HTML_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Commande Vocale Voiture</title>
    <style>
        body { font-family: 'Segoe UI', sans-serif; text-align: center; background: #121212; color: white; padding-top: 50px; margin: 0; }
        h2 { color: #007bff; font-size: 26px; }
        .mic-btn { width: 130px; height: 130px; border-radius: 50%; background: #dc3545; border: none; color: white; font-size: 40px; cursor: pointer; box-shadow: 0 0 20px rgba(220,53,69,0.4); margin-top: 20px; transition: all 0.3s; }
        .recording { background: #28a745; box-shadow: 0 0 25px rgba(40,167,69,0.8); transform: scale(1.05); }
        #word { font-size: 24px; color: #ffc107; font-weight: bold; margin-top: 20px; min-height: 30px; }
        #status { margin-top: 15px; font-size: 15px; color: #aaa; }
    </style>
</head>
<body>
    <h2>Contrôle Vocale Smartphone</h2>
    <p style="color: #666; margin: 0;">Reconnaissance vocale native via le navigateur</p>
    <button id="micBtn" class="mic-btn" onclick="toggleListening()">🎙️</button>
    <div id="word">... En attente d'un ordre ...</div>
    <p id="status">Clique sur le micro pour parler.</p>

    <script>
        let recognition; let isListening = false;
        const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;
        if (!SpeechRecognition) {
            alert("Navigateur non compatible. Utilise Google Chrome sur Android.");
        } else {
            recognition = new SpeechRecognition(); recognition.lang = 'fr-FR'; recognition.continuous = true;
            recognition.interimResults = false;
            recognition.onresult = (event) => {
                const textEntendu = event.results[event.results.length - 1][0].transcript.toLowerCase().trim();
                document.getElementById('word').innerText = `"${textEntendu}"`;
                if (textEntendu.includes('go') || textEntendu.includes('avance')) fetch('/go');
                else if (textEntendu.includes('stop') || textEntendu.includes('arrête')) fetch('/stop');
                else if (textEntendu.includes('droite')) fetch('/droite');
                else if (textEntendu.includes('gauche')) fetch('/gauche');
            };
            recognition.onend = () => { if(isListening) recognition.start(); }; 
        }
        function toggleListening() {
            if (!recognition) return;
            if (isListening) { recognition.stop(); isListening = false; document.getElementById('micBtn').classList.remove('recording'); }
            else { recognition.start(); isListening = true; document.getElementById('micBtn').classList.add('recording'); }
        }
    </script>
</body>
</html>
)=====";

// Routage de l'API Web HTTP REST 
void handleRoot() { server.send(200, "text/html", HTML_PAGE); }
void handleGo() { if(mesCommandes.mode == 1){ mesCommandes.vitesse = 120; mesCommandes.angle = 0; mesCommandes.frein = 0; } server.send(200, "text/plain", "OK"); }
void handleStop() { if(mesCommandes.mode == 1){ mesCommandes.vitesse = 0; mesCommandes.angle = 0; mesCommandes.frein = 1; } server.send(200, "text/plain", "OK"); }
void handleDroite() { if(mesCommandes.mode == 1){ mesCommandes.vitesse = 100; mesCommandes.angle = 45; mesCommandes.frein = 0; } server.send(200, "text/plain", "OK"); }
void handleGauche() { if(mesCommandes.mode == 1){ mesCommandes.vitesse = 100; mesCommandes.angle = -45; mesCommandes.frein = 0; } server.send(200, "text/plain", "OK"); }

// Réception (ESP-NOW) 
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    if (len == sizeof(struct_feedback)) { 
        memcpy(&retourVoiture, incomingData, sizeof(retourVoiture)); 
    }
}

void setup() {
    Serial.begin(115200);
    
    // Configuration des GPIOs en entrées logiques avec Pull-Up interne activé
    pinMode(PIN_BTN_ACCEL, INPUT_PULLUP);
    pinMode(PIN_BTN_DECEL, INPUT_PULLUP);
    pinMode(PIN_BTN_MODE, INPUT_PULLUP);
    
    // 1. Initialisation de la couche physique I2C (SDA = G21, SCL = G22)
    Wire.begin(21, 22);
    lcd.init(); lcd.backlight();
    
    // 2. Initialisation et configuration de la centrale inertielle MPU6050
    if (!mpu.begin()) { while (1) { delay(10); } } // Blocage de sécurité si le bus I2C échoue
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ); // Filtrage passe-bas matériel anti-vibrations

    // 3. Configuration de la pile RF : Mode Hybride (Point d'Accès + Station)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("Voiture_Micro_IA", "ensa2026"); // SSID et mot de passe de l'IHM Smartphone

    // 4. Initialisation du protocole de communication sans fil ESP-NOW (Liaison Manette-Voiture)
    if (esp_now_init() == ESP_OK) { 
        esp_now_register_recv_cb(onDataRecv); 
    }

    // Appairage Point à Point de l'esclave cible (Voiture)
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, adresseVoiture, 6);
    peerInfo.channel = 0; peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
    
    // 5. Mapping des endpoints du serveur Web
    server.on("/", handleRoot); server.on("/go", handleGo); server.on("/stop", handleStop);
    server.on("/droite", handleDroite); server.on("/gauche", handleGauche);
    server.begin();

    // État initial Fail-Safe (Véhicule sécurisé et à l'arrêt complet)
    mesCommandes.mode = 0; mesCommandes.vitesse = 0; mesCommandes.frein = 1; mesCommandes.angle = 0;
}

void loop() {
    // Traitement des requêtes HTTP des clients connectés (Smartphone)
    server.handleClient();

    // Gestion anti-rebond logicielle pour le bouton de changement de Mode
    if (digitalRead(PIN_BTN_MODE) == LOW) {
        mesCommandes.mode++;
        if (mesCommandes.mode > 2) mesCommandes.mode = 0; 
        mesCommandes.vitesse = 0; mesCommandes.angle = 0; mesCommandes.frein = 1; // Sécurité à chaque changement
        delay(400); 
    }

    // MODE 0 : TRAITEMENT DE LA NAVIGATION MANUELLE
    if (mesCommandes.mode == 0) { 
       
        if (millis() - chronoVitesse >= 50) {
            chronoVitesse = millis();
            if (digitalRead(PIN_BTN_ACCEL) == HIGH) { 
                mesCommandes.frein = 0;
                if (mesCommandes.vitesse < 250) mesCommandes.vitesse += 5; 
            } 
            else if (digitalRead(PIN_BTN_DECEL) == HIGH ) {
                if (mesCommandes.vitesse > 5) { mesCommandes.vitesse -= 5; mesCommandes.frein = 0; } 
                else { mesCommandes.vitesse = 0; mesCommandes.frein = 1; }
            }
        }
        
        // Lecture des données brutes de l'accéléromètre et calcul trigonométrique de l'angle d'inclinaison
        sensors_event_t a, g, temp; mpu.getEvent(&a, &g, &temp);
        float inclinaisonDeg = (atan2(a.acceleration.y, a.acceleration.z)) * 180.0 / M_PI;
        
        // Saturation logicielle de la consigne d'angle entre -90° et +90°
        mesCommandes.angle = (int8_t)constrain((int)inclinaisonDeg, -90, 90);
    }

    // Envoi de la trame de commande via ESP-NOW vers la passerelle de la voiture
    esp_now_send(adresseVoiture, (uint8_t *) &mesCommandes, sizeof(mesCommandes));

    // RAFRAICHISSEMENT LCD 
    if (millis() - chronoAffichage >= 200) {
        chronoAffichage = millis();
        
        // Conversion des Ticks/s en km/h
        float vitesseKmh = retourVoiture.vitesse_reelle * 0.03675;

        lcd.clear(); 
        
        //Ligne 1 : Statut Système
        lcd.setCursor(0, 0);
        if (mesCommandes.mode == 0) lcd.print("M:MANU");
        else if (mesCommandes.mode == 1) lcd.print("M:VOIX");
        else lcd.print("M:AUTO");
        
        lcd.setCursor(8, 0); 
        lcd.print("C:"); lcd.print(mesCommandes.vitesse); // Affichage de la consigne théorique
        if(mesCommandes.frein) lcd.print("(F)");
        
        //  Ligne 2 : Données Capteurs
        lcd.setCursor(0, 1);
        lcd.print("A:"); lcd.print(mesCommandes.angle); lcd.print((char)223); 
        
        lcd.setCursor(7, 1); 
        lcd.print("V:"); 
        lcd.print(vitesseKmh, 2); 
        lcd.print("kmh");        
    }
    delay(5); 
}
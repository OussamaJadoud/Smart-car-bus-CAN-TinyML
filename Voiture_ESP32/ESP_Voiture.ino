#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <driver/twai.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>

// Affectation des broches pour le bus CAN et le lecteur de carte SD
#define CAN_TX_PIN GPIO_NUM_5   
#define CAN_RX_PIN GPIO_NUM_4   
#define SD_CS_PIN  22  

// Structure de la trame de commande envoyee a l'Arduino
typedef struct __attribute__((packed)) struct_commande {
    byte mode; byte vitesse; byte frein; int8_t angle; 
} struct_commande;

// Structure du retour de vitesse envoye par l'Arduino
typedef struct __attribute__((packed)) struct_feedback {
    byte vitesse_reelle;
} struct_feedback;

struct_commande ordresRecus;
struct_feedback feedbackNano;

// Structure pour stocker une ligne de commande lue sur la carte SD
struct Instruction {
    char action; // 'A' (Avance), 'D' (Droite), 'G' (Gauche), 'S' (Stop)
    int valeur;  // Distance ou angle cible
};

// Variables pour l'automate de commande du mode autonome SD
Instruction listeTrajet[30]; 
int totalEtapes = 0;
int etapeActuelle = 0;
bool scriptCharge = false;
long progressionInterne = 0; 

uint8_t adresseManette[] = {0x3C, 0x71, 0xBF, 0x0F, 0xBA, 0xBC}; 
unsigned long chronoLog = 0;
bool nouvelleDonneeEspNow = false;

// Fonction d'envoi des ordres de mouvement a l'Arduino Nano via le Bus CAN
void envoyerTrameCAN(byte mode, byte vit, byte fr, int8_t ang) {
    twai_message_t message;
    message.identifier = 0x100; // ID de commande fixe
    message.extd = 0; 
    message.data_length_code = 4; // Contient 4 octets de donnees
    message.data[0] = mode; 
    message.data[1] = vit; 
    message.data[2] = fr; 
    message.data[3] = (uint8_t)ang;          
    twai_transmit(&message, pdMS_TO_TICKS(10)); // Timeout de transmission de 10ms
}

// Ouverture, lecture et traitement du script de trajectoire de la carte SD
void chargerTrajetSD() {
    File file = SD.open("/SYSTEM-BOOT/CMD.txt"); // Lecture du fichier texte
    if (!file) {
        Serial.println("[SD] ERREUR CRITIQUE : Impossible d'ouvrir /SYSTEM-BOOT/CMD.txt");
        return;
    }
    
    totalEtapes = 0;
    while (file.available() && totalEtapes < 30) {
        String ligne = file.readStringUntil('\n');
        ligne.trim();
        if (ligne.length() >= 3) {
            listeTrajet[totalEtapes].action = ligne.charAt(0); // Extraction de la commande (lettre)
            listeTrajet[totalEtapes].valeur = ligne.substring(2).toInt(); // Extraction de la valeur
            totalEtapes++;
        }
    }
    file.close();
    Serial.print("[SD] Script lu ! Nombre d'instructions enregistrées : ");
    Serial.println(totalEtapes);
    etapeActuelle = 0;
    progressionInterne = 0;
    scriptCharge = true;
}

// Fonction de reception radio ESP-NOW declenchee a l'arrivee d'un message de la manette
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    if (len == sizeof(struct_commande)) {
        byte ancienMode = ordresRecus.mode;
        memcpy(&ordresRecus, incomingData, sizeof(ordresRecus));
        nouvelleDonneeEspNow = true;
        
        // Reinitialisation de l'automate si l'utilisateur bascule sur le Mode Autonome
        if (ordresRecus.mode == 2 && ancienMode != 2) {
            scriptCharge = false;
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    // Configuration du Wi-Fi pour la communication radio avec la manette
    WiFi.mode(WIFI_STA); WiFi.disconnect();

    // Initialisation et demarrage du peripherique CAN interne a 500 kbps
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config  = TWAI_TIMING_CONFIG_500KBITS(); 
    twai_filter_config_t f_config  = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK && twai_start() == ESP_OK) {
        Serial.println("[OK] Bus CAN natif connecté.");
    }

    // Initialisation materielle du lecteur de carte SD en mode SPI (CS sur G22)
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("[SD] Échec : Vérifie le branchement sur G22, G23, G19, G18 ou le format FAT32.");
    } else {
        Serial.println("[SD] Lecteur SD initialisé avec succès sur G22.");
    }

    // Configuration et appairage radio ESP-NOW avec la manette
    if (esp_now_init() == ESP_OK) { esp_now_register_recv_cb(onDataRecv); }
    esp_now_peer_info_t peerInfo = {}; memcpy(peerInfo.peer_addr, adresseManette, 6);
    peerInfo.channel = 0; peerInfo.encrypt = false; esp_now_add_peer(&peerInfo);

    // Etat initial de securite au demarrage (Vehicule a l'arret complet)
    ordresRecus.mode = 0; ordresRecus.vitesse = 0; ordresRecus.frein = 1; ordresRecus.angle = 0;
}

void loop() {
    // Execution de la machine a etats si le Mode Autonome (2) est actif
    if (ordresRecus.mode == 2) {
        if (!scriptCharge) { chargerTrajetSD(); } // Chargement initial des ordres du fichier

        if (scriptCharge && etapeActuelle < totalEtapes) {
            char action = listeTrajet[etapeActuelle].action;
            int cible   = listeTrajet[etapeActuelle].valeur;

            if (action == 'A') { // Comportement : Avancer en ligne droite
                ordresRecus.vitesse = 120; ordresRecus.angle = 0; ordresRecus.frein = 0;
                if (progressionInterne >= (cible * 10)) { etapeActuelle++; progressionInterne = 0; }
            } 
            else if (action == 'D') { // Comportement : Pivot a Droite
                ordresRecus.vitesse = 100; ordresRecus.angle = 45; ordresRecus.frein = 0;
                if (progressionInterne >= (cible * 4)) { etapeActuelle++; progressionInterne = 0; }
            } 
            else if (action == 'G') { // Comportement : Pivot a Gauche
                ordresRecus.vitesse = 100; ordresRecus.angle = -45; ordresRecus.frein = 0;
                if (progressionInterne >= (cible * 4)) { etapeActuelle++; progressionInterne = 0; }
            } 
            else if (action == 'S') { // Comportement : Arret de securite en fin de course
                ordresRecus.vitesse = 0; ordresRecus.angle = 0; ordresRecus.frein = 1;
            }
        }
    }

    // Routage permanent des ordres actifs vers l'Arduino Nano sur le bus CAN
    envoyerTrameCAN(ordresRecus.mode, ordresRecus.vitesse, ordresRecus.frein, ordresRecus.angle);

    // Capture de la trame de feedback de vitesse emise par la Nano (ID 0x102)
    twai_message_t msgRecu;
    static bool canRecuOk = false;
    if (twai_receive(&msgRecu, pdMS_TO_TICKS(2)) == ESP_OK) {
        if (msgRecu.identifier == 0x102) {
            feedbackNano.vitesse_reelle = msgRecu.data[0];
            
            // Increment de la progression si le mode autonome utilise la carte SD
            if (ordresRecus.mode == 2 && ordresRecus.frein == 0) {
                progressionInterne += feedbackNano.vitesse_reelle;
            }
            // Renvoi direct de la vitesse reelle par radio a la manette pour mise a jour de l'affichage
            esp_now_send(adresseManette, (uint8_t *) &feedbackNano, sizeof(feedbackNano));
            canRecuOk = true;
        }
    }

    // Affichage des logs de controle sur la console serie toutes les 250 ms
    if (millis() - chronoLog >= 250) {
        chronoLog = millis();
        if (ordresRecus.mode == 2) {
            Serial.print("[AUTOMATE SD] Instruction : "); Serial.print(etapeActuelle);
            Serial.print("/") ; Serial.print(totalEtapes);
            Serial.print(" | Code Action : "); Serial.print(listeTrajet[etapeActuelle].action);
            Serial.print(" | Progression : "); Serial.println(progressionInterne);
        } else {
            Serial.print("[PASSERELLE] Mode : "); Serial.print(ordresRecus.mode == 0 ? "MANUEL" : "VOCAL");
            Serial.print(" | V: "); Serial.print(ordresRecus.vitesse);
            Serial.print(" | A: "); Serial.println(ordresRecus.angle);
        }
    }
    delay(5);
}
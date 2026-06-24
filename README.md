# 🏎️ Prototype de Véhicule Intelligent et Multiplexé

Dans le cadre de notre fin de module "Protocoles de communication", nous avons conçu une architecture distribuée multi-ECU simulant les réseaux automobiles modernes. L'objectif est de faire cohabiter des flux de données critiques en temps réel (consignes de trajectoire, commandes vocales, données capteurs) à travers différents bus industriels.

*   **Encadrant :** Pr. Anas Hatim
*   **Équipe :** Hiba Benzaghar, Oussama Jadoud, Ayoub Elbouhi, Ikram Fatene, Charaf Dine Salhy, Elouisi Mohamed

---

## 🏗️ 1. Architecture Réseau & Topologie des Bus
*   **ESP-NOW :** Liaison sans fil ultra-rapide entre la manette et la voiture.
*   **Bus CAN (500 kbps) :** Liaison filaire différentielle reliant l'ESP32 Passerelle (TJA1050) et l'Arduino Nano Moteurs (MCP2515).
*   **Bus I2C :** Utilisé dans la manette pour récupérer l'assiette du gyroscope MPU6050 et piloter l'écran LCD via le PCF8574.
*   **Bus SPI :** Utilisé pour l'accès en lecture/écriture au lecteur de carte Micro SD.

---

## 🛠️ 2. Cartographie du Matériel (Nomenclature)
1. **ESP32 (Manette) :** Maître du bus I2C, capture l'inclinaison et envoie les paquets ESP-NOW.
2. **ESP32 (Passerelle Voiture) :** Réceptionne l'ESP-NOW, écrit/lit sur la carte SD et route les commandes sur le Bus CAN.
3. **Arduino Nano (ECU Moteurs) :** Reçoit les trames CAN, calcule l'asservissement PID et pilote le driver de puissance.
4. **Capteur Optique LM393 :** Encodeur de vitesse pour la boucle de rétroaction PID.
5. **Transceivers CAN (TJA1050 & MCP2515) :** Gestion de la couche physique et protocolaire CAN.

---

## 🕹️ 3. Les 3 Modes de Fonctionnement

### 🔹 Mode Manuel (Gyroscopique)
La direction est dictée par l'angle de roulis de la manette (I2C ➔ ESP-NOW ➔ CAN). La vitesse est ajustée par des boutons poussoirs.

### 🔹 Mode Vocal (TinyML)
Reconnaissance de mots-clés (`GO`, `STOP`, `DROITE`, `GAUCHE`) directement embarquée sur le microcontrôleur grâce à un modèle de neurones MobileNetV2 optimisé (Int8) sous Edge Impulse (97.9% de précision).

### 🔹 Mode Autonome
Lecture séquentielle, parsing et exécution pas-à-pas d'un script de trajectoire stocké sur la carte Micro SD en boucle fermée avec le PID.

---

## 📂 4. Fichiers du Projet (Files)

*   `Manette_Master/Manette_Master.ino` : Code de la manette (I2C Maître & Émission ESP-NOW).
*   `Voiture_Passerelle/Voiture_Passerelle.ino` : Code de la voiture (Passerelle CAN & Gestion SPI de la carte SD).
*   `Voiture_Moteurs/Voiture_Moteurs.ino` : Code de l'Arduino Nano (Gestion moteurs & boucle PID).
*   `TinyML_Model/` : Bibliothèque d'inférence brute exportée depuis Edge Impulse.
*   `Schema_Hardware/` : Schémas électroniques du système distribué.

> ⚠️ **Note technique :** Les fichiers de l'application principale gèrent l'infrastructure des bus (CAN, ESP-NOW). La brique d'acquisition audio brute n'est pas intégrée directement dans le code final de la voiture ; l'utilisation du mode vocal requiert une adaptation pour lier un flux externe (ex: microphone d'un smartphone via WebSocket/Bluetooth) à la fonction d'inférence `run_classifier()`.

---

## 🕹️ 5. Instructions de Déploiement (Instructions)

1. **Installation du matériel :** Téléversez chaque fichier `.ino` sur son microcontrôleur respectif (ESP32 Manette, ESP32 Voiture, Arduino Nano).
2. **Inclusion de l'IA :** Importez le dossier `TinyML_Model` en tant que bibliothèque dans votre IDE Arduino.
3. **Exécution du Mode Autonome :** Placez un fichier texte de trajectoire à la racine d'une carte Micro SD et insérez-la dans le lecteur de la voiture avant le démarrage.
4. **🔧 Adaptation du Mode Vocal :** Configurez un canal d'écoute réseau sur l'ESP32 de la voiture pour intercepter les échantillons de voix de votre téléphone, puis injectez ce tampon audio directement en entrée du modèle TinyML pour piloter les trames CAN de direction.
// ----------------------------------------------
// Estação Meteorológica IoT com ESP8266
// (c) 2025 Jan Caraumã <janderson.gomes@ufrr.br>
// Atualizado em 17 de março de 2025
// ----------------------------------------------

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <time.h>
#include <Ticker.h>

// Configurações da rede
const char* ssid = "dlink";
const char* password = "";
const char* apSSID = "Estacao_Formiga";
const char* apPassword = "senha123";

// TODO IP Fixo
//TODO IPAddress apIP(192, 168, 4, 100);
//TODO IPAddress apGateway(192, 168, 4, 1);
//TODO IPAddress apSubnet(255, 255, 255, 0);

// Definições de hardware
#define DHTPIN 2
#define DHTTYPE DHT11
#define MQ135_PIN A0
#define RAIN_SENSOR_PIN 14
Adafruit_BMP085 bmp;

// Objetos globais
ESP8266WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);
Ticker secondTick;

// Variáveis de estado
bool timeValid = false;
unsigned long lastNTPUpdate = 0;
unsigned long localSeconds = 0;
String errorLog = "Logs do Sistema";

// Handlers de eventos WiFi
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;

void setup() {
    Serial.begin(115200);
    delay(100);

    // Inicialização de sensores
    dht.begin();
    if (!bmp.begin()) {
        logError("Falha no BMP180");
    }

    // Configuração WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    
    // Registro de eventos WiFi
    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

    // Tentativa de conexão com timeout
    Serial.print("Conectando a ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(500);
        Serial.print(".");
    }

    // Fallback para modo AP
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFalha na conexão. Iniciando modo AP...");
        WiFi.mode(WIFI_AP);
        // TODO WiFi.softAPConfig(apIP, apGateway, apSubnet);
        WiFi.softAP(apSSID, apPassword);
        bool apStatus = WiFi.softAP(apSSID, apPassword);
        Serial.println("Modo WiFi: " + String(WiFi.getMode()));
        Serial.println("AP iniciado: " + String(apStatus ? "OK" : "Falha"));
        Serial.print("IP do AP: ");
        Serial.println(WiFi.softAPIP());
    }

    // Configurações de tempo
    setupNTP();
    lastNTPUpdate = millis();

    // Configuração do servidor
    setupMDNS();
    server.on("/", handle_OnConnect);
    server.on("/dados", handle_JSONData);
    server.on("/logs", handle_Logs);
    server.onNotFound(handle_NotFound);
    server.begin();
    Serial.println("Servidor HTTP iniciado!");

    // Watchdog e segurança
    secondTick.attach(1, [](){ 
        if(millis() % 1000 == 0) ESP.wdtFeed();
    });
    ESP.wdtEnable(10000);
}

void loop() {
    server.handleClient();
    //if (millis() % 15000) {
    //  Serial.println("Clientes conectados: "+ String(WiFi.softAPgetStationNum()));
    //}
    MDNS.update();
    
    // Atualização do tempo local se NTP falhar
    if (!timeValid && millis() - lastNTPUpdate > 1000) {
        localSeconds++;
        lastNTPUpdate = millis();
    }
}

// ========== Funções de Suporte ==========

void onWifiConnect(const WiFiEventStationModeGotIP& event) {
    Serial.println("\nConectado ao WiFi!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    setupMDNS();
    setupNTP();
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
    Serial.println("\nDesconectado do WiFi!");
    MDNS.close();
}

void setupMDNS() {
    if (WiFi.getMode() == WIFI_AP) {
      // mDNS não funciona em modo AP
      return;
    }
    if (!MDNS.begin("estacaoformiga")) {
        logError("Falha no mDNS");
    } else {
        Serial.println("mDNS iniciado: estacaoformiga.local");
    }
}

void setupNTP() {
    configTime(-4 * 3600, 0, "pool.ntp.org", "time.nist.gov", "br.pool.ntp.org");
    timeValid = false;
    lastNTPUpdate = millis();
}

String getFormattedTime() {
    time_t now = time(nullptr);
    if (now < 86400 || !timeValid) { // Se NTP falhou
        unsigned long hours = localSeconds / 3600;
        unsigned long minutes = (localSeconds % 3600) / 60;
        unsigned long seconds = localSeconds % 60;
        return String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s (Local)";
    }

    struct tm* timeinfo = localtime(&now);
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", timeinfo);
    return String(buffer);
}

float safeDHTRead(bool isHumidity = false, int attempts = 3) {
    for(int i = 0; i < attempts; i++){
        float value = isHumidity ? dht.readHumidity() : dht.readTemperature();
        if (!isnan(value)) return value;
        delay(50);
    }
    logError("Falha na leitura do DHT");
    return NAN;
}

String readBMPPressure() {
    if (!bmp.begin()) return "null";
    
    float pressure = bmp.readPressure() / 100.0;
    if (isnan(pressure)) {
        logError("Falha no BMP180");
        return "null";
    }
    return String(pressure);
}

void logError(String message) {
    errorLog += "[" + getFormattedTime() + "] " + message + "\n";
    if (errorLog.length() > 2000) errorLog = errorLog.substring(1000);
    Serial.println("ERRO: " + message);
}

// ========== Handlers do Servidor ==========

void handle_JSONData() {
    String json = "{";
    
    // Leitura segura dos sensores
    float temp = safeDHTRead(false);
    float umid = safeDHTRead(true);
    int gas = analogRead(MQ135_PIN);
    float pressao = bmp.readPressure()/100.0;
    float altitude = bmp.readAltitude();
    int chuva = analogRead(RAIN_SENSOR_PIN);
    String chuvaStatus = classificarChuva(chuva);

    // Construção do JSON
    json += "\"status\":\"" + String(WiFi.status() == WL_CONNECTED ? "Online" : "Offline") + "\",";
    json += "\"data_hora\":\"" + getFormattedTime() + "\",";
    json += "\"temperatura\":" + (isnan(temp) ? "null" : String(temp)) + ",";
    json += "\"umidade\":" + (isnan(umid) ? "null" : String(umid)) + ",";
    json += "\"qualidade_ar\":" + String(gas) + ",";
    json += "\"pressao\":" + String(pressao) + ",";
    json += "\"altitude\":" + String(altitude) + ",";
    json += "\"chuva\":\"" + chuvaStatus + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

void handle_Logs() {
    server.send(200, "text/plain", errorLog);
}

void handle_OnConnect() {
    float temperatura = safeDHTRead(false);
    float umidade = safeDHTRead(true);
    server.send(200, "text/html", EnvioHTML(temperatura, umidade, getFormattedTime()));
}

void handle_NotFound() {
    server.send(404, "text/plain", "Recurso nao encontrado");
}

// ========== Interface Web ==========

String classificarChuva(int valor) {
    if (valor > 600) return "🌞 Sem chuva";
    if (valor > 400) return "🌧 Chuva leve";
    if (valor > 200) return "🌧️ Chuva moderada";
    return "🌧️ Chuva forte";
}

String EnvioHTML(float temperatura, float umidade, String dataHora) {
    String html = R"rawliteral(
        <!DOCTYPE html>
        <html lang="pt">
        <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1.0">
            <title>Estação Meteorológica</title>
            <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
            <style>
                /* CSS */
                * { 
                    margin: 0; 
                    padding: 0; 
                    box-sizing: border-box; 
                    font-family: 'Segoe UI', Arial, sans-serif; 
                }
                body { 
                    background: linear-gradient(135deg, #1e3c72, #2a5298);
                    display: flex; 
                    justify-content: center; 
                    align-items: center; 
                    min-height: 100vh;
                    padding: 20px;
                }
                .container { 
                    background: rgba(255, 255, 255, 0.95); 
                    padding: 25px; 
                    border-radius: 15px; 
                    box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3); 
                    width: 100%; 
                    max-width: 1200px;
                    backdrop-filter: blur(10px);
                    text-align: center;
                }
                h1 { 
                    color: #2c3e50; 
                    margin-bottom: 5px; 
                    font-size: 2.2em;
                }
                h2 { 
                    color: #7f8c8d; 
                    font-size: 1.1em; 
                    margin-bottom: 25px;
                    font-weight: normal;
                }
                .data-container { 
                    margin: 25px 0; 
                    display: grid;
                    grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
                    gap: 15px;
                }
                .data-item { 
                    background: #f8f9fa;
                    padding: 15px;
                    border-radius: 10px;
                    box-shadow: 0 2px 8px rgba(0,0,0,0.1);
                    font-size: 1.1em;
                }
                .chart-container {
                    margin: 30px 0;
                    background: white;
                    border-radius: 12px;
                    padding: 20px;
                    box-shadow: 0 4px 12px rgba(0,0,0,0.1);
                }
                canvas {
                    width: 100% !important;
                    height: 300px !important;
                }
                .button-group {
                    display: flex;
                    gap: 10px;
                    flex-wrap: wrap;
                    justify-content: center;
                    margin-top: 25px;
                }
                button { 
                    background: #3498db; 
                    color: white; 
                    border: none; 
                    padding: 12px 25px; 
                    border-radius: 8px; 
                    cursor: pointer; 
                    font-size: 0.95em;
                    transition: all 0.3s;
                    display: flex;
                    align-items: center;
                    gap: 8px;
                }
                button:hover { 
                    background: #2980b9; 
                    transform: translateY(-2px);
                    box-shadow: 0 3px 10px rgba(0,0,0,0.2);
                }
                @keyframes fadeIn { 
                    from { opacity: 0; transform: translateY(-10px); } 
                    to { opacity: 1; transform: translateY(0); } 
                }
                .container { animation: fadeIn 0.8s ease-in-out; }

                /* Responsividade */
                @media (max-width: 768px) {
                    .container { padding: 15px; }
                    canvas { height: 200px !important; }
                    h1 { font-size: 1.8em; }
                }
            </style>
            <script>
                let charts = {
                    climate: null,
                    pressure: null,
                    airQuality: null
                };

                let historico = {
                    labels: [],
                    temp: [],
                    umid: [],
                    press: [],
                    gas: []
                };

                function classificarQualidadeArJS(valor) {
                    if (valor <= 200) return "🌿 Excelente";
                    if (valor <= 400) return "😊 Boa";
                    if (valor <= 600) return "😐 Moderada";
                    if (valor <= 800) return "😷 Ruim";
                    return "🚨 Péssima";
                }

                function initCombinedChart() {
                    const ctx = document.getElementById('climateChart').getContext('2d');
                    charts.climate = new Chart(ctx, {
                        type: 'line',
                        data: {
                            labels: [],
                            datasets: [{
                                label: 'Temperatura (°C)',
                                data: [],
                                borderColor: '#e74c3c',
                                backgroundColor: 'rgba(231, 76, 60, 0.1)',
                                tension: 0.3,
                                fill: true,
                                yAxisID: 'temp'
                            },
                            {
                                label: 'Umidade (%)',
                                data: [],
                                borderColor: '#3498db',
                                backgroundColor: 'rgba(52, 152, 219, 0.1)',
                                tension: 0.3,
                                fill: true,
                                yAxisID: 'hum'
                            }]
                        },
                        options: {
                            responsive: true,
                            maintainAspectRatio: false,
                            scales: {
                                temp: {
                                    type: 'linear',
                                    position: 'left',
                                    title: { display: true, text: 'Temperatura (°C)' }
                                },
                                hum: {
                                    type: 'linear',
                                    position: 'right',
                                    title: { display: true, text: 'Umidade (%)' },
                                    grid: { drawOnChartArea: false }
                                }
                            }
                        }
                    });
                }

                function initPressureChart() {
                    const ctx = document.getElementById('pressureChart').getContext('2d');
                    charts.pressure = new Chart(ctx, {
                        type: 'line',
                        data: {
                            labels: [],
                            datasets: [{
                                label: 'Pressão Atmosférica (hPa)',
                                data: [],
                                borderColor: '#2ecc71',
                                backgroundColor: 'rgba(46, 204, 113, 0.1)',
                                tension: 0.3,
                                fill: true
                            }]
                        },
                        options: {
                            responsive: true,
                            maintainAspectRatio: false,
                            scales: {
                                y: { title: { display: true, text: 'Pressão (hPa)' } }
                            }
                        }
                    });
                }

                function initAirQualityChart() {
                    const ctx = document.getElementById('airQualityChart').getContext('2d');
                    charts.airQuality = new Chart(ctx, {
                        type: 'bar',
                        data: {
                            labels: [],
                            datasets: [{
                                label: 'Qualidade do Ar (ppm)',
                                data: [],
                                backgroundColor: '#9b59b6',
                                borderColor: '#8e44ad',
                                borderWidth: 1
                            }]
                        },
                        options: {
                            responsive: true,
                            maintainAspectRatio: false,
                            scales: {
                                y: { title: { display: true, text: 'Concentração (ppm)' } }
                            }
                        }
                    });
                }

                function atualizarDados() {
                    fetch('/dados', { timeout: 5000 })
                    .then(response => {
                        if (!response.ok) throw new Error('Erro na rede');
                        return response.json();
                    })
                    .then(data => {
                        const now = new Date();                        
                        //const timestamp = now.toLocaleTimeString([], { 
                        //    hour: '2-digit', 
                        //    minute: '2-digit'
                        //});
                        const timestamp = `${now.getDate().toString().padStart(2, '0')}/${(now.getMonth() + 1).toString().padStart(2, '0')}/${now.getFullYear()} ${now.toLocaleTimeString()}`;
                        
                        // Atualizar histórico
                        historico.labels.push(timestamp);
                        historico.temp.push(data.temperatura);
                        historico.umid.push(data.umidade);
                        historico.press.push(data.pressao);
                        historico.gas.push(data.qualidade_ar);

                        // Manter somente últimos 15 registros
                        //if(historico.labels.length > 15) {
                        //    historico.labels.shift();
                        //    historico.temp.shift();
                        //    historico.umid.shift();
                        //    historico.press.shift();
                        //    historico.gas.shift();
                        //}

                        // Atualizar gráficos
                        charts.climate.data.labels = historico.labels;
                        charts.climate.data.datasets[0].data = historico.temp;
                        charts.climate.data.datasets[1].data = historico.umid;
                        
                        charts.pressure.data.labels = historico.labels;
                        charts.pressure.data.datasets[0].data = historico.press;
                        
                        charts.airQuality.data.labels = historico.labels;
                        charts.airQuality.data.datasets[0].data = historico.gas;

                        charts.climate.update();
                        charts.pressure.update();
                        charts.airQuality.update();

                        // Atualizar dados em tempo real
                        document.getElementById('data_hora').textContent = '📅 Última atualização: ' + data.data_hora;
                        document.getElementById('tempItem').textContent = '🌡 ' + data.temperatura.toFixed(1) + ' ºC';
                        document.getElementById('humItem').textContent = '💧 ' + data.umidade.toFixed(1) + ' %';
                        document.getElementById('pressaoItem').textContent = '⏬ ' + data.pressao.toFixed(1) + ' hPa';
                        document.getElementById('altitudeItem').textContent = '🏔 ' + data.altitude.toFixed(1) + ' m';                        
                        document.getElementById('chuvaItem').textContent = '🌧 ' + data.chuva;
                        document.getElementById('qualidadeItem').textContent = `🌫 ${data.qualidade_ar} ppm (${classificarQualidadeArJS(data.qualidade_ar)})`;
                    })
                    .catch(error => {
                        console.error('Erro:', error);
                        document.getElementById('data_erro').textContent = '⚠️ Erro na atualização - ' + new Date().toLocaleTimeString();
                    });
                }

                function exportCSV() {
                    const csvContent = "Hora,Temperatura,Umidade,Pressão,QualidadeAr\n" +
                        historico.labels.map((hora, index) => 
                            `${hora},${historico.temp[index]},${historico.umid[index]},${historico.press[index]},${historico.gas[index]}`
                        ).join("\n");
                    
                    const link = document.createElement('a');
                    link.href = 'data:text/csv;charset=utf-8,' + encodeURIComponent(csvContent);
                    link.download = 'dados_estacao_' + new Date().toISOString() + '.csv';
                    link.click();
                }

                window.onload = function() {
                    initCombinedChart();
                    initPressureChart();
                    initAirQualityChart();
                    atualizarDados();
                    setInterval(atualizarDados, 15000);
                };
            </script>
        </head>
        <body>
            <div class="container">
                <h1>Monitor Ambiental</h1>
                <h2>🐜 Formiga Estação Meteorológica - Dados em Tempo Real</h2>
                
                <div class="chart-container">
                    <canvas id="climateChart"></canvas>
                </div>
                
                <div class="chart-container">
                    <canvas id="pressureChart"></canvas>
                </div>
                
                <div class="chart-container">
                    <canvas id="airQualityChart"></canvas>
                </div>

                <div class="data-container">
                    <div class="data-item temp" id="tempItem">🌡 Temperatura: )rawliteral" + String(temperatura) + R"rawliteral( ºC</div>
                    <div class="data-item hum" id="humItem">💧 Umidade: )rawliteral" + String(umidade) + R"rawliteral( %</div>
                    <div class="data-item" id="pressaoItem">⏬ Pressão: -- hPa</div>
                    <div class="data-item" id="qualidadeItem">🌫 Qualidade do Ar: --</div>
                    <div class="data-item" id="altitudeItem">🏔 Altitude: -- m</div>
                    <div class="data-item" id="chuvaItem">🌧 Chuva: --</div>
                </div>

                <div class="button-group">
                    <button onclick="exportCSV()"><span>📥</span> Exportar CSV</button>
                    <button onclick="atualizarDados()"><span>🔄</span> Atualizar Dados</button>
                    <button onclick="window.open('/logs')"><span>📋</span> Logs do Sistema</button>
                </div>
                
                <p id="data_hora" style="text-align:center; margin-top:20px; color:#7f8c8d;">📅 Última atualização: )rawliteral" + dataHora + R"rawliteral(</p>
                <p id="data_erro" style="text-align:center; margin-top:20px; color:#7f8c8d;">Sem notificações.</p>
            </div>
        </body>
        </html>
    )rawliteral";
    return html;
}

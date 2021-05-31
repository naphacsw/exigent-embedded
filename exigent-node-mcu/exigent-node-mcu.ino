
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#define _TASK_MICRO_RES
#include <TaskScheduler.h>
#include <RingBuf.h>

#define PLAY_ENABLE_PIN 5
#define TASK_QUEUE_SIZE 100;

const char* ssid = "Chong_2.4G";
const char* password = "0818262171";
const char* mqtt_server = "broker.netpie.io";
const int mqtt_port = 1883;
const char* mqtt_Client = "87d59e71-0028-4777-80e1-50102cc48d57";
const char* mqtt_username = "XUuJuFj6agQ5dqYfnurLQpxjLdsx7Y4X";
const char* mqtt_password = "K~RCo*b0xk~0SO8!QL)2ws2BfBS1(NkK";
String backend_host = "http://api.pattanachai.xyz/api/";
WiFiClient espClient;
PubSubClient client(espClient);

struct ExigentTask {
  // type 0: download
  // type 1: play
  int type; 
  String audioId;
  String taskId;
  boolean ok;
}currentTask;
RingBuf<ExigentTask, 100> task_queue;

const uint8_t responseTopic[] = "@msg/task/complete";
char str_buff[100], taskTopic[50];

void callback(char* topic,byte* payload, unsigned int length) {
  Serial.println("Message arrived");
  String msg;
  for(int i=0;i<length;++i) msg += (char) payload[i];
  sprintf(str_buff, "[%s]: %s", topic, msg.c_str());
  Serial.println(str_buff);
  if( strcmp(topic, taskTopic) == 0 ) {
    ExigentTask newTask;
    createTask(newTask,msg);
    bool inserted = task_queue.push(&newTask);
    if(!inserted) {
      Serial.println("Task queue is full!");
      return;
    }
    taskReceived(newTask.taskId);
  }
}

void createTask(ExigentTask &newTask, String msg) {
  int indx = msg.indexOf(' ');
  String type = msg.substring(0,indx);

  if(type == String("Download")) newTask.type = 0;
  else if(type == String("Play")) newTask.type = 1;
  
  msg = msg.substring(indx+1);
  indx = msg.indexOf(' ');
  newTask.audioId = msg.substring(0,indx);
  newTask.taskId = msg.substring(indx+1);

  newTask.ok = false;
}

void taskReceived(String taskId){
  client.publish("@msg/task/received",taskId.c_str());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection…");
    if (client.connect(mqtt_Client, mqtt_username, mqtt_password)) {
      Serial.println("connected");
      client.subscribe(taskTopic);
    }
    else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println("try again in 5 seconds");
      delay(5000);
    }
  }
}


Scheduler ts;

void networkLoop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}

Task tNetwork(1 * TASK_MILLISECOND, TASK_FOREVER, &networkLoop, &ts, true, NULL, NULL); 

File file;
WiFiClient wifiClient;
HTTPClient http;
Stream * downloadStream;

void onlineCheckTask() {
  client.publish("@msg/online",mqtt_Client);
}

Task tOnline(60000 * TASK_MILLISECOND, TASK_FOREVER, &onlineCheckTask, &ts, true, NULL, NULL);

void downloadTask();

Task tDownload(50, TASK_FOREVER, &downloadTask, &ts, false, NULL, &onTaskComplete);

int32_t download_size;
bool preDownloadTask() {
  String fileName = currentTask.audioId+".wav";
  if(SD.exists(fileName)){
    Serial.println(String("File already existed: ") + fileName);
    return false;
  }
 
  file = SD.open(fileName, FILE_WRITE);
  http.begin(wifiClient, backend_host+"audios/"+currentTask.audioId+"/file");
  if(http.GET() != 200) {
    Serial.println(String("Can\'t download file") + fileName);
    return false;
  }
  downloadStream = http.getStreamPtr();
  download_size = http.getSize();
  return true;
}


void downloadTask() {
  if(downloadStream->available()) {
    file.write(downloadStream->read());
    --download_size;
  }else if(download_size <= 0) {
    postDownloadTask();
    tDownload.disable();
  }
}

void postDownloadTask() {
  http.end();
  file.close();
  currentTask.ok = true;
  Serial.println("Download Completed!");
}

bool preAudioTask() {
  String fileName = currentTask.audioId+".wav";
  if(!SD.exists(fileName)){
    Serial.println(String("File does not existed: ") + fileName);
    return false;
  }
  file = SD.open(fileName, FILE_READ);
  file.seek(44);
  digitalWrite(PLAY_ENABLE_PIN,LOW);
  return true;
}

void audioTask() {
  if( file.available() ){
    Serial.write(file.read());
  }else {
    file.seek(44);
  }
}

void responseTask() {
  if(Serial.available() && Serial.read() == 'R') {
    currentTask.ok = true;
    Serial.println("RESPONSE GOT");
  }
}
Task tResponse ( 50, 10000, &responseTask, &ts, false ,NULL, &onTaskComplete);

void postAudioTask() {
  file.close();
  digitalWrite(PLAY_ENABLE_PIN,HIGH);
  Serial.println("Finished playing audio");
  tResponse.restart();
  tResponse.enable();
}

Task tAudio ( 63, 320000, &audioTask, &ts, false ,NULL, &postAudioTask);

int program_state=0;
void onTaskComplete() {
  program_state = 0;
  if(currentTask.ok)  client.publish("@msg/task/complete",currentTask.taskId.c_str());
}

void processTask() {
  if( program_state != 0 || task_queue.isEmpty()) return;
    Serial.println( "Task picked");
    task_queue.pop(currentTask);
    if(currentTask.type == 0) {
      bool shouldProceed = preDownloadTask();
      if(shouldProceed) {
        Serial.println("Start downloading...");
        program_state = 1;
        tDownload.enable();
      }
    } else if (currentTask.type == 1) {
      bool shouldProceed = preAudioTask();
      if(shouldProceed) {
        Serial.println("Start playing audio...");
        program_state = 1;
        tAudio.restart();
        tAudio.enable();
      }
    }
}
Task tProcessTask(1*TASK_MILLISECOND, TASK_FOREVER, &processTask, &ts, true, NULL, NULL);

void setup() {
  pinMode(PLAY_ENABLE_PIN,OUTPUT);
  digitalWrite(PLAY_ENABLE_PIN,HIGH);

  strcpy(taskTopic,"@msg/task/");
  strcat(taskTopic, mqtt_Client);
  
  Serial.begin(161000);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  
  while (!SD.begin(4)) {
    Serial.println("SD card initialization failed! trying again...");
    delay(1000);
  }
  Serial.println("SD card initialized!");
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  ts.execute();
}

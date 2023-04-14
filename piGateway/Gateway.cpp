/*
RFM69 Gateway RFM69 pushing the data to the mosquitto server
by Alexandre Bouillot

License:  CC-BY-SA, https://creativecommons.org/licenses/by-sa/2.0/
Date:  2016/11/24
File: Gateway.c

This sketch receives RFM wireless data and forwards it to Mosquitto relay

The messages are published with the format RFM/<network number>/<node_id>/up/<sensor_id><var>

It is also, theoretically, possible to publist to Mosquitto Topic RFM/<network_number>/<node_id>/down/<sensor_id>
which will parse the message and deliver it to the requested network node.


Adjust network configuration to your setup in the file networkconfig.h
*/

//general --------------------------------
#define SERIAL_BAUD   115200

//RFM69  ----------------------------------
#include "rfm69.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "networkconfig.h"

RFM69 *rfm69;

typedef struct {		
	unsigned long messageWatchdog;
	unsigned long messageSent;
	unsigned long messageReceived;
	unsigned long ackRequested;
	
	unsigned long ackReceived;
	unsigned long ackMissed;
	
	unsigned long ackCount;
} 
Stats;
Stats theStats;

typedef struct {
	uint8_t networkId;
	uint8_t nodeId;
	uint8_t frequency; // RF69_433MHZ RF69_868MHZ RF69_915MHZ
	uint8_t keyLength; // set to 0 for no encryption
	char key[16];
	bool isRFM69HW;
	bool promiscuousMode;
	unsigned long messageWatchdogDelay; // maximum time between two message before restarting radio module
	}
Config;
Config theConfig;

// Mosquitto---------------
#include <mosquitto.h>

/* How many seconds the broker should wait between sending out
* keep-alive messages. */
#define KEEPALIVE_SECONDS 60
/* Hostname and port for the MQTT broker. */
#define BROKER_HOSTNAME "192.168.1.25"
#define BROKER_PORT 1883

#define MQTT_ROOT "RFM" // KJS TODO -- this also appears to be hardcoded elsewhere
#define MQTT_CLIENT_ID "rfm69Client"
#define MQTT_RETRY 500

int sendMQTT = 0;

typedef struct {		
	short           nodeID; 
	short			sensorID;
	char value[32];
// long   var1_usl; 
//	float           var2_float; 
//	float			var3_float;	
} 
Payload;
Payload theData;

typedef struct SensorNode : Payload {
	int             rssi;
}
SensorNode;
SensorNode sensorNode;

static void die(const char *msg);
static long millis(void);
static void hexDump (char *desc, void *addr, int len, int bloc);

static void initRfm(RFM69 *rfm);

static bool set_callbacks(struct mosquitto *m);
static bool connect(struct mosquitto *m);
static int run_loop(struct mosquitto *m);

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val);
static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val);
static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val);
static void MQTTSendChar(struct mosquitto* _client, int node, int sensor, char* val);

#ifdef DAEMON
#define LOG(...) do { syslog(LOG_INFO, __VA_ARGS__); } while (0)
#define LOG_E(...) do { syslog(LOG_ERR, __VA_ARGS__); } while (0)
#else
#ifdef DEBUG
#define DEBUG1(expression)  fprintf(stderr, expression)
#define DEBUG2(expression, arg)  fprintf(stderr, expression, arg)
#define DEBUGLN1(expression)  
#define LOG(...) do { printf(__VA_ARGS__); } while (0)
#define LOG_E(...) do { printf(__VA_ARGS__); } while (0)
#else
#define DEBUG1(expression)
#define DEBUG2(expression, arg)
#define DEBUGLN1(expression)
#define LOG(...) 
#define LOG_E(...)
#endif
#endif

static void uso(void) {
	fprintf(stderr, "Use:\n Simply use it without args :D\n");
	exit(1);
}

int main(int argc, char* argv[]) {

	if (argc != 1) uso();

        LOG("RF69_IRQ_PIN %d\n", RF69_IRQ_PIN);

#ifdef DAEMON
	//Adapted from http://www.netzmafia.de/skripten/unix/linux-daemon-howto.html
	pid_t pid, sid;

	openlog("Gatewayd", LOG_PID, LOG_USER);


	pid = fork();
	if (pid < 0) {
		LOG_E("fork failed");
		exit(EXIT_FAILURE);
	}
	/* If we got a good PID, then
		 we can exit the parent process. */
	if (pid > 0) {
		LOG("Child spawned, pid %d\n", pid);
		exit(EXIT_SUCCESS);
	}

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		LOG_E("setsid failed");
		exit(EXIT_FAILURE);
	}
        
	/* Change the current working directory */
	if ((chdir("/")) < 0) {
	  LOG_E("chdir failed");
	  exit(EXIT_FAILURE);
	}
        
	/* Close out the standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
#endif //DAEMON

	// Mosquitto ----------------------
	struct mosquitto *m = mosquitto_new(MQTT_CLIENT_ID, true, null);
	if (m == NULL) { die("init() failure\n"); }

	if (!set_callbacks(m)) { die("set_callbacks() failure\n"); }
	if (!connect(m)) { die("connect() failure\n"); }

	//RFM69 ---------------------------
	theConfig.networkId = NWC_NETWORK_ID;
	theConfig.nodeId = NWC_NODE_ID;
	theConfig.frequency = NWC_FREQUENCY;
	theConfig.keyLength = NWC_KEY_LENGTH;
	memcpy(theConfig.key, NWC_KEY, NWC_KEY_LENGTH);
	theConfig.isRFM69HW = NWC_RFM69H;
	theConfig.promiscuousMode = NWC_PROMISCUOUS_MODE;
	theConfig.messageWatchdogDelay = NWC_WATCHDOG_DELAY; // 1800 seconds (30 minutes) between two messages 

	rfm69 = new RFM69();
	LOG("RFM init %d / %d / %d\n", theConfig.frequency,theConfig.nodeId,theConfig.networkId);

	if (!rfm69->initialize(theConfig.frequency,theConfig.nodeId,theConfig.networkId))
	{
		die("rfm69->initialize failed");
	}
	initRfm(rfm69);

	// Mosquitto subscription for delivery *to* RFM nodes  ---------
	char subscriptionMask[128];
	sprintf(subscriptionMask, "%s/%03d/+/down/+", MQTT_ROOT, theConfig.networkId);
	LOG("Subscribing to Mosquitto 'down' topic...: %s\n", subscriptionMask);
	mosquitto_subscribe(m, NULL, subscriptionMask, 0);
	
	LOG("setup complete\n");

	return run_loop(m);
}  // end of setup

/* Loop until it is explicitly halted or the network is lost, then clean up. */
static int run_loop(struct mosquitto *m) {
	int res;
	long lastMess; 
	for (;;) {
		res = mosquitto_loop(m, 10, 1);

		// No messages have been received withing MESSAGE_WATCHDOG interval
		if (millis() > lastMess + theConfig.messageWatchdogDelay) {
			LOG("=== Message WatchDog ===\n");
			theStats.messageWatchdog++;
			// re-initialise the radio
			initRfm(rfm69);
			// reset watchdog
			lastMess = millis();
		}
		
		if (rfm69->receiveDone()) {
			// record last message received time - to compute radio watchdog
			lastMess = millis();
			theStats.messageReceived++;
			
			// store the received data localy, so they can be overwited
			// This will allow to send ACK immediately after
			uint8_t data[RF69_MAX_DATA_LEN]; // recv/xmit buf, including header & crc bytes
			uint8_t dataLength = rfm69->DATALEN;
			memcpy(data, (void *)rfm69->DATA, dataLength);
			uint8_t theNodeID = rfm69->SENDERID;
			uint8_t targetID = rfm69->TARGETID; // should match _address
			uint8_t PAYLOADLEN = rfm69->PAYLOADLEN;
			uint8_t ACK_REQUESTED = rfm69->ACK_REQUESTED;
			uint8_t ACK_RECEIVED = rfm69->ACK_RECEIVED; // should be polled immediately after sending a packet with ACK request
			int16_t RSSI = rfm69->RSSI; // most accurate RSSI during reception (closest to the reception)

			if (ACK_REQUESTED && targetID == theConfig.nodeId) {
				// When a node requests an ACK, respond to the ACK
				// but only if the Node ID is correct
				theStats.ackRequested++;
				rfm69->sendACK();
				
/*
				if (theStats.ackCount++%3==0) {
					// and also send a packet requesting an ACK (every 3rd one only)
					// This way both TX/RX NODE functions are tested on 1 end at the GATEWAY

					usleep(3000);  //need this when sending right after reception .. ?
					theStats.messageSent++;
					if (rfm69->sendWithRetry(theNodeID, "ACK TEST", 8)) { // 3 retry, over 200ms delay each
						theStats.ackReceived++;
						LOG("Pinging node %d - ACK - ok!", theNodeID);
					}
					else {
						theStats.ackMissed++;
						LOG("Pinging node %d - ACK - nothing!", theNodeID);
					}
				}
*/
			}//end if radio.ACK_REQESTED
	
			// LOG("Node [%d] to Node [%d]\n", theNodeID, targetID);

			if (dataLength != sizeof(Payload)) {
				LOG("Invalid payload received, not matching Payload struct! %d - %d\r\n", dataLength, sizeof(Payload));
				hexDump(NULL, data, dataLength, sizeof(Payload));		
			} else {
				theData = *(Payload*)data; //assume radio.DATA actually contains our struct and not something else

				//save it for mosquitto:
				sensorNode.nodeID = theData.nodeID;
				sensorNode.sensorID = theData.sensorID;
				strcpy(sensorNode.value, theData.value);
				//sensorNode.var1_usl = theData.var1_usl;
				//sensorNode.var2_float = theData.var2_float;
				//sensorNode.var3_float = theData.var3_float;
				//sensorNode.rssi = RSSI;

				LOG("Received Node ID = %d Device ID = %d val = %s\n",
					sensorNode.nodeID,
					sensorNode.sensorID,
					sensorNode.value
					//sensorNode.var1_usl,
					//sensorNode.rssi,
					//sensorNode.var2_float,
					//sensorNode.var3_float
				);
				if (sensorNode.nodeID == theNodeID)
					sendMQTT = 1;
				else {
					hexDump(NULL, data, dataLength, sizeof(Payload));
				}
			}  
		} //end if radio.receive

		if (sendMQTT == 1) {
			MQTTSendChar(m, sensorNode.nodeID, sensorNode.sensorID, sensorNode.value);

			//send var4_int, RSSI
//			MQTTSendInt(m, sensorNode.nodeID, sensorNode.sensorID, 4, sensorNode.rssi);

			sendMQTT = 0;
		}//end if sendMQTT
	}

	mosquitto_destroy(m);
	(void)mosquitto_lib_cleanup();

	if (res == MOSQ_ERR_SUCCESS) {
		return 0;
	} else {
		return 1;
	}
}

static void initRfm(RFM69 *rfm) {
	rfm->restart(theConfig.frequency,theConfig.nodeId,theConfig.networkId);
	if (theConfig.isRFM69HW)
		rfm->setHighPower(); //uncomment only for RFM69HW!
	if (theConfig.keyLength)
		rfm->encrypt(theConfig.key);
	rfm->promiscuous(theConfig.promiscuousMode);
	LOG("Listening at %d Mhz...\n", theConfig.frequency==RF69_433MHZ ? 433 : theConfig.frequency==RF69_868MHZ ? 868 : 915);
}

/* Fail with an error message. */
static void die(const char *msg) {
	fprintf(stderr, "%s", msg);
	exit(1);
}

static long millis(void) {
	struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((tv.tv_sec) * 1000 + tv.tv_usec/1000.0) + 0.5;
	}

	
/* Binary Dump utility function */
#define MAX_BLOC 16
const unsigned char hex_asc[] = "0123456789abcdef";
static void hexDump (char *desc, void *addr, int len, int bloc) {
    int i, lx, la, l, line;
	long offset = 0;
    unsigned char hexbuf[MAX_BLOC * 3 + 1];	// Hex part of the data (2 char + 1 space)
	unsigned char ascbuf[MAX_BLOC + 1];	// ASCII part of the data
    unsigned char *pc = (unsigned char*)addr;
	unsigned char ch;
	
	// nothing to output
	if (!len)
		return;

	// Limit the line length to MAX_BLOC
	if (bloc > MAX_BLOC) 
		bloc = MAX_BLOC;
		
	// Output description if given.
    if (desc != NULL)
		LOG("%s:\n", desc);
	
	line = 0;
	do
		{
		l = len - (line * bloc);
		if (l > bloc)
			l = bloc;
	
		for (i=0, lx = 0, la = 0; i < l; i++) {
			ch = pc[i];
			hexbuf[lx++] = hex_asc[((ch) & 0xF0) >> 4];
			hexbuf[lx++] = hex_asc[((ch) & 0xF)];
			hexbuf[lx++] = ' ';
		
			ascbuf[la++]  = (ch > 0x20 && ch < 0x7F) ? ch : '.';
			}
	
		for (; i < bloc; i++) {
			hexbuf[lx++] = ' ';
			hexbuf[lx++] = ' ';
			hexbuf[lx++] = ' ';
		}	
		// nul terminate both buffer
		hexbuf[lx++] = 0;
		ascbuf[la++] = 0;
	
		// output buffers
		LOG("%04x %s %s\n", line * bloc, hexbuf, ascbuf);
		
		line++;
		pc += bloc;
		}
	while (line * bloc < len);
}

static void MQTTSendChar(struct mosquitto* _client, int node, int sensor, char* value) {
	char buff_topic[128];
	char buff_message[128];

	sprintf(buff_topic, "%s/%03d/%02d/up/%01d", MQTT_ROOT, theConfig.networkId, node, sensor, value);
	sprintf(buff_message, "%s", value);
  	LOG("Publish %s %s\n", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val) {
	char buff_topic[128];
	char buff_message[128];

	sprintf(buff_topic, "%s/%03d/%02d/up/%01d%01d", MQTT_ROOT, theConfig.networkId, node, sensor, var);
	sprintf(buff_message, "%04d%", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val) {
	char buff_topic[128];
	char buff_message[128];

	sprintf(buff_topic, "%s/%03d/%02d/up/%01d%01d", MQTT_ROOT, theConfig.networkId, node, sensor, var);
	sprintf(buff_message, "%u", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
	}

static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val) {
	char buff_topic[128];
	char buff_message[128];

	sprintf(buff_topic, "%s/%03d/%02d/up/%01d%01d", MQTT_ROOT, theConfig.networkId, node, sensor, var);
	snprintf(buff_message, 12, "%f", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, buff_topic, strlen(buff_message), buff_message, 0, false);

	}

// Handing of Mosquitto messages
void callback(char* topic, uint8_t* payload, unsigned int length) {
	// handle message arrived
	LOG("Mosquitto Callback\n");
}


/* Connect to the network. */
static bool connect(struct mosquitto *m) {
	int res = mosquitto_connect(m, BROKER_HOSTNAME, BROKER_PORT, KEEPALIVE_SECONDS);
	LOG("Connect return %d\n", res);
	return res == MOSQ_ERR_SUCCESS;
}

/* Callback for successful connection: add subscriptions. */
static void on_connect(struct mosquitto *m, void *udata, int res) {
	if (res == 0) {   /* success */
		LOG("Mosquitto connection success\n");
	} else {
		die("Mosquitto connection refused\n");
	}
}

/*
 Handle a message that just arrived via one of the subscriptions.

  KJS this is intended to allow applications to send a message out to an RFM69 via a queue
      not needed by me right now :-)


 */
static void on_message(struct mosquitto *m, void *udata,
const struct mosquitto_message *msg) {
	if (msg == NULL) { return; }
	LOG("-- got message @ %s: (%d, QoS %d, %s) '%s'\n",
		msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
		msg->payload);
		
	if (strlen((const char *)msg->topic) < strlen(MQTT_ROOT) + 2 + 3 + 1) {return; }	// message is smaller than "RFM/xxx/x" so likey invalid

	Payload data;
	uint8_t network;

	sscanf(msg->topic, "RFM/%d/%d/%d", &network, &data.nodeID, &data.sensorID);
	if (strncmp(msg->topic, MQTT_ROOT, strlen(MQTT_ROOT)) == 0 && network == theConfig.networkId) {
		
		// extract the target network and the target node from the topic
		sscanf(msg->topic, "RFM/%d/%d/%d", &network, &data.nodeID, &data.sensorID);
		
		if (network == theConfig.networkId) {
			// only process the messages to our network
		
			sscanf((const char *)msg->payload, "%ld,%f,%f", data.nodeID, data.sensorID, &data.value);
			
			LOG("Received message for Node ID = %d Device ID = %d  varl = %s\n",
				data.nodeID,
				data.sensorID,
				data.value
			);

			theStats.messageSent++;
			if (rfm69->sendWithRetry(data.nodeID,(const void*)(&data),sizeof(data))) {
				LOG("Message sent to node %d ACK", data.nodeID);
				theStats.ackReceived++;
				}
			else {
				LOG("Message sent to node %d NAK", data.nodeID);
				theStats.ackMissed++;
			}
		}
	}
}

/* A message was successfully published. */
static void on_publish(struct mosquitto *m, void *udata, int m_id) {
  	LOG(" -- published successfully\n");
}

/* Successful subscription hook. */
static void on_subscribe(struct mosquitto *m, void *udata, int mid,
		int qos_count, const int *granted_qos) {
  	LOG(" -- subscribed successfully\n");
}

/* Register the callbacks that the mosquitto connection will use. */
static bool set_callbacks(struct mosquitto *m) {
	mosquitto_connect_callback_set(m, on_connect);
	mosquitto_publish_callback_set(m, on_publish);
	mosquitto_subscribe_callback_set(m, on_subscribe);
	mosquitto_message_callback_set(m, on_message);
	return true;
}


#include <cstring>
#include <memory>
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <respeaker.h>
#include <libevdev.h>
#include <mosquitto.h>
#include <sstream>
#include <vector>
#include <chain_nodes/alsa_collector_node.h>
#include <chain_nodes/vep_aec_beamforming_node.h>
#include <chain_nodes/snowboy_1b_doa_kws_node.h>
#include <chain_nodes/snips_1b_doa_kws_node.h>


#include "json.hpp"
#include "toml.h"

extern "C"
{
#include <sndfile.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

#define SIGNED_SIZEOF(x)	((int) sizeof (x))

#define TOGGLEHOTWORDONTOPIC "hermes/hotword/toggleOn"
#define TOGGLEHOTWORDOFFTOPIC "hermes/hotword/toggleOff"
#define OUTPUTACTIVETOPIC "respeakercorev2d/outputactive"


const char *hotword_model_path = "./hotword_models/";

std::vector<std::string> hotword_models = {
    "Olga_Chris.pmdl",
    //"snowboy.umdl",
    //"computer.umdl",
    "alexa_02092017.umdl"
    //"OK_Google.pmdl"
};

using namespace std;
using namespace respeaker;

#define BLOCK_SIZE_MS    8
static bool stop = false;

unique_ptr<ReSpeaker> respeaker_ptr;
string source = "sysdefault:CARD=seeed8micvoicec";
bool enable_agc = false;
int agc_level = 10;
string mic_type;
string siteid = "default";
string mqttHost = "localhost";
int mqttPort = 1883;

std::vector<std::string> stringSplit(const std::string& s, char seperator)
{
   std::vector<std::string> output;

    std::string::size_type prev_pos = 0, pos = 0;

    while((pos = s.find(seperator, pos)) != std::string::npos)
    {
        std::string substring( s.substr(prev_pos, pos-prev_pos) );

        output.push_back(substring);

        prev_pos = ++pos;
    }

    output.push_back(s.substr(prev_pos, pos-prev_pos)); // Last word

    return output;
}

void hexDump(const void *data, const size_t size)
{
    /* dumps size bytes of *data to stdout. Looks like:
     * [0000] 75 6E 6B 6E 6F 77 6E 20
     *                  30 FF 00 00 00 00 39 00 unknown 0.....9.
     * (in a single line of course)
     */

    unsigned char *p = (unsigned char*)data;
    unsigned char c;
    unsigned int n;
    char bytestr[10] = {0};
    char addrstr[10] = {0};
    char hexstr[ 16*10 + 5] = {0};
    char charstr[16*1 + 5] = {0};

    for(n=1;n<=size;n++) {
        if (n%16 == 1) {
            /* store address for this line */
            snprintf(addrstr, sizeof(addrstr), "%.4x", (unsigned int)(p-(unsigned char*)data) );
        }

        c = *p;
        if (isalnum(c) == 0) {
            c = '.';
        }

        /* store hex str (for left side) */
        snprintf(bytestr, sizeof(bytestr), "%02X ", *p);
        strncat(hexstr, bytestr, sizeof(hexstr)-strlen(hexstr)-1);

        /* store char str (for right side) */
        snprintf(bytestr, sizeof(bytestr), "%c", c);
        strncat(charstr, bytestr, sizeof(charstr)-strlen(charstr)-1);

        if(n%16 == 0)
        {
            /* line completed */
            printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);

            hexstr[0] = 0;
            charstr[0] = 0;
        }
        else if(n%8 == 0)
        {
            /* half line: add whitespaces */
            strncat(hexstr, "  ", sizeof(hexstr)-strlen(hexstr)-1);
            strncat(charstr, " ", sizeof(charstr)-strlen(charstr)-1);
        }
        p++; /* next byte */
    }

    if (strlen(hexstr) > 0)
    {
        /* print rest of buffer if not empty */
        printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
    }
}

uint64_t timeSinceEpochMillisec() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool caseInSensStringCompareCpp11(std::string & str1, std::string &str2)
{
	return ((str1.size() == str2.size()) && std::equal(str1.begin(), str1.end(), str2.begin(), [](char & c1, char & c2) {
	    return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
    }));
}

void SignalHandler(int signal)
{
    cerr << "Caught signal " << signal << ", terminating..." << endl;
    stop = true;
}

void mqtt_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{
    if(message->payloadlen)
    {
        //We always require a payload.  So if we don't get one we just ignore this message

        //lets parse the paylod (and extract any common info that might be needed)
        std::string stringSiteId = "";
        try
        {
            auto parsedPayload = nlohmann::json::parse((const char*)message->payload);

            //Try and resolve the site ID from the payload
            auto siteIdValue = parsedPayload.find("siteId");

            if (siteIdValue != parsedPayload.end())
            {
                stringSiteId = (*siteIdValue);
            }
        }
        catch (...)
        {
        }

        size_t messageTopicLength = strlen(message->topic);

        bool hotwordToggleOnCommand = false;
        bool hotwordToggleOffCommand = false;
        if (strncasecmp(TOGGLEHOTWORDONTOPIC, message->topic, messageTopicLength) == 0)
        {
            hotwordToggleOnCommand = true;
        }
        else if (strncasecmp(TOGGLEHOTWORDOFFTOPIC, message->topic, messageTopicLength) == 0)
        {
            hotwordToggleOffCommand = true;
        }
        else if (strncasecmp(OUTPUTACTIVETOPIC, message->topic, messageTopicLength) == 0)
        {
            //TODO: deal with switching to and from the BGM modes of listening/triggering
        }
        else
        {
            //Unknown topic.
            return;
        }

        if (hotwordToggleOnCommand || hotwordToggleOffCommand)
        {
            //Check to make sure this is for the right site id before we react to it.
            if (caseInSensStringCompareCpp11(siteid, stringSiteId))
            {
                //Site ID matches.  Sort out what we are being asked to do.

                //Get the chain state data and sort out if we really need to change our state
                ChainSharedData *chainstateData = respeaker_ptr->GetChainSharedDataPtr();

                if (hotwordToggleOnCommand)
                {
                    printf("Toggle hotword ON for our siteid\n");
                    //We only need to ask the chain to change state if we are passivly listening
                    if (chainstateData && chainstateData->state == ChainState::LISTEN_QUIETLY)
                    {
                        respeaker_ptr->SetChainState(ChainState::WAIT_TRIGGER_QUIETLY);
                    }
                }
                else if (hotwordToggleOffCommand)
                {
                    printf("Toggle hotword OFF for our siteid\n");
                    //We only need to change state if we are waiting for a trigger
                    if (chainstateData && chainstateData->state == ChainState::WAIT_TRIGGER_QUIETLY)
                    {
                        respeaker_ptr->SetChainState(ChainState::LISTEN_QUIETLY);
                    }
                }
            }
        }
    }
    else
    {
        //Since there was no payload we will just ignore this.  We are expecting a specific bit of data.
    }


    if(message->payloadlen) {
		printf("%s %s\n", message->topic, message->payload);
	}else{
		printf("%s (null)\n", message->topic);
	}
	fflush(stdout);
}

void mqtt_connect_callback(struct mosquitto *mosq, void *userdata, int result)
{
	int i;
	if(!result)
    {
        //Subscribve to hotword control messages
		mosquitto_subscribe(mosq, NULL, TOGGLEHOTWORDONTOPIC, 2);
        mosquitto_subscribe(mosq, NULL, TOGGLEHOTWORDOFFTOPIC, 2);
        printf("Subscribed to hotword control topics\n");

        //subscribe to media playback messages (for third party notifications of audio being output)
        mosquitto_subscribe(mosq, NULL, OUTPUTACTIVETOPIC, 2);
        printf("Subscribed to 3d party media playback messages\n");
	}
    else
    {
		fprintf(stderr, "MQTT Connect failed\n");
	}
}

void mqtt_subscribe_callback(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos)
{
	int i;

	printf("Subscribed (mid: %d): %d", mid, granted_qos[0]);
	for(i=1; i<qos_count; i++){
		printf(", %d", granted_qos[i]);
	}
	printf("\n");
}

void mqtt_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	/* Pring all log messages regardless of level. */
	printf("%s\n", str);
}


/*==============================================================================
*/

typedef struct
{	sf_count_t offset, length ;
	unsigned char data [16 * 1024] ;
} VIO_DATA ;

static sf_count_t
vfget_filelen (void *user_data)
{	VIO_DATA *vf = (VIO_DATA *) user_data ;

	return vf->length ;
} /* vfget_filelen */

static sf_count_t
vfseek (sf_count_t offset, int whence, void *user_data)
{	VIO_DATA *vf = (VIO_DATA *) user_data ;

	switch (whence)
	{	case SEEK_SET :
			vf->offset = offset ;
			break ;

		case SEEK_CUR :
			vf->offset = vf->offset + offset ;
			break ;

		case SEEK_END :
			vf->offset = vf->length + offset ;
			break ;
		default :
			break ;
		} ;

	return vf->offset ;
} /* vfseek */

static sf_count_t
vfread (void *ptr, sf_count_t count, void *user_data)
{	VIO_DATA *vf = (VIO_DATA *) user_data ;

	/*
	**	This will break badly for files over 2Gig in length, but
	**	is sufficient for testing.
	*/
	if (vf->offset + count > vf->length)
		count = vf->length - vf->offset ;

	memcpy (ptr, vf->data + vf->offset, count) ;
	vf->offset += count ;

	return count ;
} /* vfread */

static sf_count_t
vfwrite (const void *ptr, sf_count_t count, void *user_data)
{	VIO_DATA *vf = (VIO_DATA *) user_data ;

	/*
	**	This will break badly for files over 2Gig in length, but
	**	is sufficient for testing.
	*/
	if (vf->offset >= SIGNED_SIZEOF (vf->data))
		return 0 ;

	if (vf->offset + count > SIGNED_SIZEOF (vf->data))
		count = sizeof (vf->data) - vf->offset ;

	memcpy (vf->data + vf->offset, ptr, (size_t) count) ;
	vf->offset += count ;

	if (vf->offset > vf->length)
		vf->length = vf->offset ;

    return count ;
} /* vfwrite */

static sf_count_t
vftell (void *user_data)
{	VIO_DATA *vf = (VIO_DATA *) user_data ;

	return vf->offset ;
} /* vftell */


/*==============================================================================
*/

static void help(const char *argv0) {
    cout << "alsa_aloop_test [options]" << endl;
    cout << "A demo application for librespeaker." << endl << endl;
    cout << "  -h, --help                               Show this help" << endl;
    cout << "  -s, --source=SOURCE_NAME                 The alsa source (microphone) to connect to" << endl;
    cout << "  -t, --type=MIC_TYPE                      The MICROPHONE TYPE, support: CIRCULAR_6MIC_7BEAM, LINEAR_6MIC_8BEAM, LINEAR_4MIC_1BEAM, CIRCULAR_4MIC_9BEAM" << endl;
    cout << "  -g, --agc=NEGTIVE INTEGER                The target gain level of output, [-31, 0]" << endl;
    //cout << "      --siteid=SITE_ID                     The Snips site id" << endl;
}

int main(int argc, char *argv[])
{
    // Configures signal handling.
    struct sigaction sig_int_handler;
    sig_int_handler.sa_handler = SignalHandler;
    sigemptyset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;
    sigaction(SIGINT, &sig_int_handler, NULL);
    sigaction(SIGTERM, &sig_int_handler, NULL);

    // parse opts
    int c;
    static const struct option long_options[] = {
        {"help",         0, NULL, 'h'},
        {"source",       1, NULL, 's'},
        {"type",         1, NULL, 't'},
        {"agc",          1, NULL, 'g'},
        //{"siteid",       1, NULL, 1000},
        {NULL,           0, NULL,  0}
    };

    while ((c = getopt_long(argc, argv, "hs:o:k:t:g:w", long_options, NULL)) != -1) {
        switch (c) {
        case 'h' :
            help(argv[0]);
            return 0;
        case 's':
            source = string(optarg);
            break;
        case 't':
            mic_type = string(optarg);
            break;
        case 'g':
            enable_agc = true;
            agc_level = stoi(optarg);
            if ((agc_level > 31) || (agc_level < -31)) agc_level = 31;
            if (agc_level < 0) agc_level = (0-agc_level);
            break;
        /*case 1000:
            siteid = string(optarg);
            break;*/
        default:
            return 0;
        }
    }

    //load the snips config file so we can pull out any of the details in it we need
    std::ifstream ifs("/etc/snips.toml");
    toml::ParseResult pr = toml::parse(ifs);

    if (pr.valid())
    {
        const toml::Value& parsedValues = pr.value;

        const toml::Value* mqttSettingString = parsedValues.find("snips-common.mqtt");
        if (mqttSettingString && mqttSettingString->is<std::string>())
        {
            std::vector<std::string> splitMQTTDetails = stringSplit(mqttSettingString->as<std::string>(), ':');
            if (splitMQTTDetails.size() == 2)
            {
                mqttHost = splitMQTTDetails[0];
                mqttPort = atoi(splitMQTTDetails[1].c_str());
            }
        }

        const toml::Value* audioBindString = parsedValues.find("snips-audio-server.bind");
        if (audioBindString && audioBindString->is<std::string>())
        {
            std::vector<std::string> splitAudioBindOptions = stringSplit(audioBindString->as<std::string>(), '@');
            if (splitAudioBindOptions.size() == 2)
            {
                siteid = splitAudioBindOptions[0];
            }
        }
    }

    //Setup libevdev to get the button pushes
    struct libevdev *dev = NULL;
    int fd;
    int rc = 1;

    fd = open("/dev/input/event0", O_RDONLY|O_NONBLOCK);
    rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        fprintf(stderr, "Failed to init libevdev (%s)\n", strerror(-rc));
        exit(1);
    }

    //Startup the MQTT connection
    struct mosquitto *mosq = NULL;
    bool clean_session = true;

    printf("Connecting to MQTT server: %s:%d\n", mqttHost.c_str(), mqttPort);
    printf("Using siteid: %s\n", siteid.c_str());

	mosquitto_lib_init();
    mosq = mosquitto_new(NULL, clean_session, NULL);
	if(!mosq){
		fprintf(stderr, "Error: Unable to allocate the MQTT connection - Out of memory.\n");
		return 1;
	}

    //mosquitto_log_callback_set(mosq, mqtt_log_callback);
	mosquitto_connect_callback_set(mosq, mqtt_connect_callback);
	mosquitto_message_callback_set(mosq, mqtt_message_callback);
	//mosquitto_subscribe_callback_set(mosq, mqtt_subscribe_callback);

    if(mosquitto_connect(mosq, mqttHost.c_str(), mqttPort, 60))
    {
		fprintf(stderr, "Error: Unable to connect to the MQTT server.\n");
		return 1;
	}
    mosquitto_loop_start(mosq);

    std::stringstream audioFrameTopic;
    audioFrameTopic << "hermes/audioServer/" << siteid << "/audioFrame";

    unique_ptr<AlsaCollectorNode> collector;
    unique_ptr<VepAecBeamformingNode> vep_beam;
    unique_ptr<Snowboy1bDoaKwsNode> kws;
    //unique_ptr<Snips1bDoaKwsNode> kws;


    collector.reset(AlsaCollectorNode::Create(source, 48000, false));
    vep_beam.reset(VepAecBeamformingNode::Create(MicType::CIRCULAR_6MIC_7BEAM, true, 6, true));

    //build up our model string and sensitivities string
    std::stringstream hotwordModelString;
    std::stringstream hotwordSensitivitiesString;

    int hotwordIndexCount = 0;
    for (std::vector<std::string>::iterator it = hotword_models.begin(); it != hotword_models.end(); it++)
    {
        if (it != hotword_models.begin())
        {
            hotwordModelString << ",";
            hotwordSensitivitiesString << ",";
        }
        hotwordModelString << hotword_model_path << *it;
        hotwordSensitivitiesString << "0.4";

        printf("%d: %s\n", hotwordIndexCount++, (*it).c_str());
    }

    //printf("%s\n", hotwordModelString.str().c_str());
    //printf("%s\n", hotwordSensitivitiesString.str().c_str());
    //exit(1);

    kws.reset(Snowboy1bDoaKwsNode::Create("/usr/share/respeaker/snowboy/resources/common.res", hotwordModelString.str().c_str(),hotwordSensitivitiesString.str().c_str()));
    //kws.reset(Snips1bDoaKwsNode::Create("/usr/share/snips/assistant/custom_hotword", 0.5, enable_agc, false));

    //Snips deals with notification of the transfer state with its own messages
    //So we can disable the auto transfer states here
    kws->DisableAutoStateTransfer();
    kws->SetDoAecWhenListen(true);

    if (enable_agc)
    {
        kws->SetAgcTargetLevelDbfs(agc_level);
        cout << "AGC = -"<< agc_level<< endl;
    }
    else {
        cout << "Disable AGC" << endl;
    }

    vep_beam->Uplink(collector.get());
    kws->Uplink(vep_beam.get());

    respeaker_ptr.reset(ReSpeaker::Create(TRACE_LOG_LEVE));

    // collector->SetThreadPriority(50);
    // vep_1beam->SetThreadPriority(99);
    // kws->SetThreadPriority(51);
    //vep_1beam->BindToCore(3);
    //collector->BindToCore(3);
    //kws->BindToCore(2);

    respeaker_ptr->RegisterChainByHead(collector.get());
    respeaker_ptr->RegisterOutputNode(kws.get());
    respeaker_ptr->RegisterDirectionManagerNode(kws.get());
    respeaker_ptr->RegisterHotwordDetectionNode(kws.get());

    if (!respeaker_ptr->Start(&stop))
    {
        cout << "Can not start the respeaker node chain." << endl;
        return -1;
    }

    // You should call this after respeaker->Start()
    //aloop->SetMaxBlockDelayTime(250);

    size_t num_channels = respeaker_ptr->GetNumOutputChannels();
    int rate = respeaker_ptr->GetNumOutputRate();
    cout << "num channels: " << num_channels << ", rate: " << rate << endl;


    string framedata_chunk1;
    string framedata_chunk2;

    //Setup the sndfile virtual IO that we use to get wav data to mqtt
    VIO_DATA        vio_data;
    SF_VIRTUAL_IO   vio;
	SNDFILE         *sndfile;
	SF_INFO         sfinfo;

    vio.get_filelen = vfget_filelen ;
	vio.seek = vfseek ;
	vio.read = vfread ;
	vio.write = vfwrite ;
	vio.tell = vftell ;

	vio_data.offset = 0 ;
	vio_data.length = 0 ;

	memset (&sfinfo, 0, sizeof (sfinfo)) ;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	sfinfo.channels = num_channels;
	sfinfo.samplerate = rate;

    SF_CHUNK_INFO chunk_time ;
    memset (&chunk_time, 0, sizeof (chunk_time)) ;
    snprintf (chunk_time.id, sizeof (chunk_time.id), "time") ;
    chunk_time.id_size = 4 ;
    chunk_time.datalen = 8;
    chunk_time.data = malloc(chunk_time.datalen);

    SF_CHUNK_INFO chunk_replay_request_id ;
    memset (&chunk_replay_request_id, 0, sizeof (chunk_replay_request_id)) ;
    snprintf (chunk_replay_request_id.id, sizeof (chunk_replay_request_id.id), "rpid") ;
    chunk_replay_request_id.id_size = 4 ;
    chunk_replay_request_id.datalen = 0;

    SF_CHUNK_INFO chunk_replay_remaning_frames ;
    memset (&chunk_replay_remaning_frames, 0, sizeof (chunk_replay_remaning_frames)) ;
    snprintf (chunk_replay_remaning_frames.id, sizeof (chunk_replay_remaning_frames.id), "rpfr") ;
    chunk_replay_remaning_frames.id_size = 4 ;
    chunk_replay_remaning_frames.datalen = 0;

    int tick;
    int hotword_index = 0, hotword_trigger_count = 0;
    int dir = 0;
    bool vad_status = false;

    bool activationTriggered = false;

    bool physicalButtonTrigger = false;
    uint16_t previousPhysicalButtonValue = 0;

    uint64_t lasttimestamp = timeSinceEpochMillisec();

    uint64_t averagedifftotal = 0;
    uint64_t averagediffcount = 0;
    while (!stop)
    {
        ChainSharedData *chainstateData = respeaker_ptr->GetChainSharedDataPtr();

        //Do the physical button checking
        {
            physicalButtonTrigger = false;

            int eveventcount = 0;
            do
            {
                struct input_event ev;
                rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                eveventcount++;

                if (rc == 0)
                {
                    if (ev.type == EV_KEY && ev.code == KEY_F24)
                    {
                        if (previousPhysicalButtonValue != ev.value && ev.value == 0)
                        {
                            physicalButtonTrigger = true;
                        }
                        previousPhysicalButtonValue = ev.value;

                        printf("Event: %s %s %d\n",
                                libevdev_event_type_get_name(ev.type),
                                libevdev_event_code_get_name(ev.type, ev.code),
                                ev.value);
                    }
                }
            } while (rc == 0 && eveventcount < 20);
        }

        if (chainstateData && (chainstateData->state == ChainState::WAIT_TRIGGER_QUIETLY || chainstateData->state == ChainState::WAIT_TRIGGER_WITH_BGM))
        {
            //Check to see if we have any hotword triggered
            hotword_index = respeaker_ptr->DetectHotword();

            if (hotword_index > 0 || physicalButtonTrigger)
            {
                bool bailoutAndIgnore = false;
                std::string hotwordId = "default";
                //Hotword has been detected (or the physical button was pushed).  Time to notify snips
                if (physicalButtonTrigger)
                {
                    //if we are triggered by a physical button there is no direction.  Just hardcode to 0
                    dir = 0;
                    hotwordId = "physicalbutton";

                    //since we can have multiple button pushes we want to ignore any after the first one
                    //we do that by using the chain state (so we also as a side effect ignore button pushes after a hotword activation)
                    //Check to see if we are in state LISTEN_QUIETLY
                    if (chainstateData && (chainstateData->state == ChainState::LISTEN_QUIETLY || chainstateData->state == ChainState::LISTEN_WITH_BGM))
                    {
                        //We have already had a hotword (or physical button) trigger.  So just bail out
                        bailoutAndIgnore = true;
                    }
                }
                else
                {
                    //When trigged by the hotword engine we can fetch the DOA
                    dir = respeaker_ptr->GetDirection();

                    //Sort out the wakeword that was used and reflect that.
                    int activatedHotwordIndex = hotword_index - 1;
                    if (activatedHotwordIndex < hotword_models.size())
                    {
                        hotwordId = hotword_models.at(activatedHotwordIndex);
                    }
                    else
                    {
                        hotwordId = "unknownhotword";
                    }
                }

                if (!bailoutAndIgnore)
                {
                    //We build this up manually since snips apparenly needs the json in a specific order to function
                    std::stringstream payload;
                    payload << "{"
                        << "\"siteId\":\"" << siteid << "\","
                        << "\"modelId\":\"" << hotwordId << "\","
                        << "\"modelVersion\":\"1\","
                        << "\"modelType\":" << "\"universal\"" << ","
                        << "\"currentSensitivity\":" << 0.5 << ","
                        << "\"direction\":" << dir
                        << "}";

                    std::stringstream topic;
                    topic << "hermes/hotword/" << hotwordId << "/detected";

                    //push the hotword activation message to MQTT
                    mosquitto_publish(mosq, NULL, topic.str().c_str(), payload.str().length(), payload.str().c_str(), 2, false);

                    hotword_trigger_count++;
                    cout << "hotword: " << hotwordId << ", direction: " << dir << ", hotword_count = " << hotword_trigger_count << endl;

                    //Switch the chain to the listen mode
                    respeaker_ptr->SetChainState(ChainState::LISTEN_QUIETLY);
                }
            }
        }

        //lets process audio data

        //We get data from librespeaker in chunks of 1280 bytes (which corresponds to 640 audio frames)
        //We need to send it to snips in 512 byte chunks (256 audio frames)
        //So to do that we ask librespeaker for data twice to get a total of 2560 bytes (1280 audio frames)
        //Which we can then split up into 5 audio frames and send along at the proper size
        uint64_t timestamp_chunk1 = timeSinceEpochMillisec();
        framedata_chunk1 = respeaker_ptr->Listen();
        const int16_t *framedata_chunk1_pointer = (const int16_t *)(framedata_chunk1.data());
        //char *framedataPointer_tmp = (char *)malloc(2560); //(const int16_t *)(framedata.data());
        //memset(framedataPointer_tmp, 1, 1280);
        /*char counter = 0;
        for (int g = 0; g < 1280; g++)
        {
            framedataPointer_tmp[g] = counter++;
        }*/
        //const int16_t *framedata_chunk1_pointer = (const int16_t *)framedataPointer_tmp;



        uint64_t timestamp_chunk2 = timeSinceEpochMillisec();
        framedata_chunk2 = respeaker_ptr->Listen();
        const int16_t *framedata_chunk2_pointer = (const int16_t *)(framedata_chunk2.data());
        //char *framedataPointer2_tmp = (char *)malloc(2560); //(const int16_t *)(framedata.data());
        //memset(framedataPointer2_tmp, 2, 1280);
        /*counter = 0;
        for (int g = 0; g < 1280; g++)
        {
            framedataPointer2_tmp[g] = counter++;
        }*/
        //const int16_t *framedata_chunk2_pointer = (const int16_t *)framedataPointer2_tmp;


        int framesProcessed = 0;

        int bytesToProcess = framedata_chunk1.length() + framedata_chunk2.length();
        if (bytesToProcess == 2560)
        {
            int packetCount = 0;
            while (bytesToProcess > 0)
            {
                uint64_t datatimestamp = 0;
                //reset the buffer we use for the wav data
                vio_data.offset = 0;
                vio_data.length = 0;

                if ((sndfile = sf_open_virtual (&vio, SFM_WRITE, &sfinfo, &vio_data)) == NULL)
                {
                    printf ("\n\nLine %d : sf_open_write failed with error : ", __LINE__) ;
                    fflush (stdout) ;
                    puts (sf_strerror (NULL)) ;
                    exit (1) ;
                };

                if (framesProcessed < 512)
                {
                    //Audio packet #1 and #2
                    if (framesProcessed == 0)
                    {
                        //Timestamp for packet 1 is timestamp_chunk1
                        //nothing to do as the timestamp is the base
                    }
                    else
                    {
                        //Timestamp for packet 2 is timestamp_chunk1 + 8ms
                        timestamp_chunk1 += 8;
                    }

                    datatimestamp = timestamp_chunk1;
                    memcpy(chunk_time.data, &timestamp_chunk1, chunk_time.datalen);
                    //write out the extra meta data
                    sf_set_chunk(sndfile, &chunk_time);

                    //Data comes from first chunk
                    sf_writef_short(sndfile, framedata_chunk1_pointer + framesProcessed, 256);
                }
                else if (framesProcessed == 512)
                {
                    //Audio packet #3
                    //Timestamp comes from timestamp_chunk1 and is 8ms after packet 2.
                    timestamp_chunk1 += 8;
                    datatimestamp = timestamp_chunk1;
                    memcpy(chunk_time.data, &timestamp_chunk1, chunk_time.datalen);
                    //write out the extra meta data
                    sf_set_chunk(sndfile, &chunk_time);

                    //Data come from first and second chunk
                    sf_writef_short(sndfile, framedata_chunk1_pointer + framesProcessed, 128);
                    sf_writef_short(sndfile, framedata_chunk2_pointer + (framesProcessed - 512), 128);
                }
                else if (framesProcessed > 512)
                {
                    //Audio packet #4 and #5
                    if (framesProcessed == 768)
                    {
                        //Timestamp for packet 4 is based on timestamp_chunk2 but it is 4ms in the future (since we have half a packet in packet 3)
                        timestamp_chunk2 += 4;
                    }
                    else
                    {
                        //Timestamp for packet 5 is 8ms after packet 4
                        timestamp_chunk2 += 8;
                    }
                    datatimestamp = timestamp_chunk2;
                    memcpy(chunk_time.data, &timestamp_chunk2, chunk_time.datalen);
                    //write out the extra meta data
                    sf_set_chunk(sndfile, &chunk_time);

                    //Data comes from second chunk
                    sf_writef_short(sndfile, framedata_chunk2_pointer + (framesProcessed - 640), 256);
                }

                //Every loop through we have processed 512 bytes (256 audio frames)
                bytesToProcess -= 512;
                framesProcessed += 256;

                //Close up the virtual file so we can use that data to send to mqtt
                sf_close(sndfile);

                //Stuff the data in our circular buffer based on the timestamp for replay
                //TODO
                //storeBuffer(datatimestamp, vio_data.data, vio_data.length);

                //Debug output
                //printf("Packet #%d------------------\n", ++packetCount);
                //hexDump(vio_data.data, vio_data.length);

                //if we are supposed to be sending this audio data along then do so
                //We only send to MQTT when we have been triggered by a hotword or physical button push.  That way
                //we aren't sending tons of data over the network if the mqtt server is remote
                if (chainstateData && (chainstateData->state == ChainState::LISTEN_QUIETLY || chainstateData->state == ChainState::LISTEN_WITH_BGM))
                {
                    //Send the data to MQTT
                    mosquitto_publish(mosq, NULL, audioFrameTopic.str().c_str(), vio_data.length, vio_data.data, 2, false);
                }
            }
        }
        else
        {
            //we didn't get enough data from librespeaker.  For now we'll ignore this and maybe make it an error later
        }

        if (tick++ % 10 == 0)
        {
            std::cout << "collector: " << collector->GetQueueDeepth() << ", vep_beam: " << vep_beam->GetQueueDeepth() << ", kws: " << kws->GetQueueDeepth() << std::endl;
        }
    }

    cout << "stopping the respeaker worker thread..." << endl;
    respeaker_ptr->Pause();
    respeaker_ptr->Stop();

    //Clean up the libevdev descriptor
    close(fd);

    free(chunk_time.data); chunk_time.data = NULL;

    //Cleanup the mqtt connection
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosq = NULL;
    mosquitto_lib_cleanup();

    cout << "cleanup done." << endl;
    return 0;
}

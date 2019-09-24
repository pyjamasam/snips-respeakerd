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


#include "json.hpp"

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
    "snowboy.umdl",
    "computer.umdl",
    "jarvis.umdl",
    "alexa_02092017.umdl",
    "OK_Google.pmdl"
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
    cout << "      --siteid=SITE_ID                     The Snips site id" << endl;
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
        {"siteid",       1, NULL, 1000},
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
        case 1000:
            siteid = string(optarg);
            break;
        default:
            return 0;
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

    if(mosquitto_connect(mosq, "localhost", 1883, 60))
    {
		fprintf(stderr, "Error: Unable to connect to the MQTT server.\n");
		return 1;
	}
    mosquitto_loop_start(mosq);

    std::stringstream audioFrameTopic;
    audioFrameTopic << "hermes/audioServer/" << siteid << "/audioFrame";

    unique_ptr<AlsaCollectorNode> collector;
    unique_ptr<VepAecBeamformingNode> vep_beam;
    unique_ptr<Snowboy1bDoaKwsNode> snowboy_kws;

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

    //due to a bug in librespeaker we need to add another sensitivity on here
    hotwordSensitivitiesString << ",0";

//    printf("%s\n", hotwordModelString.str().c_str());
//    printf("%s\n", hotwordSensitivitiesString.str().c_str());
//    exit(1);

    snowboy_kws.reset(Snowboy1bDoaKwsNode::Create("/usr/share/respeaker/snowboy/resources/common.res", hotwordModelString.str().c_str(),hotwordSensitivitiesString.str().c_str()));

    //Snips deals with notification of the transfer state with its own messages
    //So we can disable the auto transfer states here
    snowboy_kws->DisableAutoStateTransfer();
    snowboy_kws->SetDoAecWhenListen(true);

    if (enable_agc)
    {
        snowboy_kws->SetAgcTargetLevelDbfs(agc_level);
        cout << "AGC = -"<< agc_level<< endl;
    }
    else {
        cout << "Disable AGC" << endl;
    }

    vep_beam->Uplink(collector.get());
    snowboy_kws->Uplink(vep_beam.get());

    respeaker_ptr.reset(ReSpeaker::Create(TRACE_LOG_LEVE));

    // collector->SetThreadPriority(50);
    // vep_1beam->SetThreadPriority(99);
    // snowboy_kws->SetThreadPriority(51);
    //vep_1beam->BindToCore(3);
    //collector->BindToCore(3);
    //snowboy_kws->BindToCore(2);

    respeaker_ptr->RegisterChainByHead(collector.get());
    respeaker_ptr->RegisterOutputNode(snowboy_kws.get());
    respeaker_ptr->RegisterDirectionManagerNode(snowboy_kws.get());
    respeaker_ptr->RegisterHotwordDetectionNode(snowboy_kws.get());


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


    string framedata;
    int frames;

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

    int tick;
    int hotword_index = 0, hotword_count = 0;
    int dir = 0;
    bool vad_status = false;

    bool activationTriggered = false;

    bool physicalButtonTrigger = false;
    uint16_t previousPhysicalButtonValue = 0;

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

        //Process any audio data thats inflight
        framedata = respeaker_ptr->DetectHotword(hotword_index);
        //vad_status = respeaker_ptr->GetVad();

        //printf("framedata length: %d\n", framedata.length());
        //Sort out what mode we are in and what we have to do based on that mode
        if (chainstateData && (chainstateData->state == ChainState::LISTEN_QUIETLY || chainstateData->state == ChainState::LISTEN_WITH_BGM))
        {
            //We are listening to the user speaking.  Lets just forward this audio on to mqtt for asr processing.
            ////TODO: Check VAD and stop if we no longer have active voice detection
            //printf("Vad status:%d\n", vad_status);

            //Write out our audio data into the wav container.
            frames = framedata.length() / (sizeof(int16_t) * num_channels);

            //reset the buffer we use for the wav data
            vio_data.offset = 0;
	        vio_data.length = 0;

            //Open up our virtual file that we use to to sort out the raw sound data
            //and get it into a format that snips wants
            if ((sndfile = sf_open_virtual (&vio, SFM_WRITE, &sfinfo, &vio_data)) == NULL)
            {
                printf ("\n\nLine %d : sf_open_write failed with error : ", __LINE__) ;
                fflush (stdout) ;
                puts (sf_strerror (NULL)) ;
                exit (1) ;
            };

            sf_writef_short(sndfile, (const int16_t *)(framedata.data()), frames);
            sf_close(sndfile);

            //push the message to MQTT
            mosquitto_publish(mosq, NULL, audioFrameTopic.str().c_str(), vio_data.length, vio_data.data, 2, false);

        }
        else if (chainstateData && (chainstateData->state == ChainState::WAIT_TRIGGER_QUIETLY || chainstateData->state == ChainState::WAIT_TRIGGER_WITH_BGM))
        {
            //We are waiting for our trigger so lets check to see if one occured and process as required
             if (hotword_index >= 1 || physicalButtonTrigger)
            {
                bool bailout = false;
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
                        bailout = true;
                    }
                }
                else
                {
                    //When trigged by the hotword engine we can fetch the DOA
                    dir = respeaker_ptr->GetDirection();

                    //TODO: sort out the wakeword that was used and reflect that.
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

                if (!bailout)
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

                    //push the message to MQTT
                    mosquitto_publish(mosq, NULL, topic.str().c_str(), payload.str().length(), payload.str().c_str(), 2, false);

                    hotword_count++;
                    cout << "hotword: " << hotwordId << ", direction: " << dir << ", hotword_count = " << hotword_count << endl;

                    //Switch the chain to the listen mode
                    respeaker_ptr->SetChainState(ChainState::LISTEN_QUIETLY);
                }
            }
        }

        if (tick++ % 20 == 0)
        {
            std::cout << "collector: " << collector->GetQueueDeepth() << ", vep_beam: " << vep_beam->GetQueueDeepth() << ", snowboy_kws: " << snowboy_kws->GetQueueDeepth() << std::endl;
        }
    }

    cout << "stopping the respeaker worker thread..." << endl;
    respeaker_ptr->Pause();
    respeaker_ptr->Stop();

    //Clean up the libevdev descriptor
    close(fd);

    //Cleanup the mqtt connection
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosq = NULL;
    mosquitto_lib_cleanup();

    cout << "cleanup done." << endl;
    return 0;
}

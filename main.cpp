/*
	jackiir
	IIR parametric equalizer for JACK without GUI 
	
	Copyright (C) 2014 adiblol
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <jack/jack.h>

using namespace std;

#define debug(...) fprintf(stderr, __VA_ARGS__)

typedef jack_default_audio_sample_t sample_t;
typedef sample_t* sample_buffer_t;
int samplerate = 0;
bool bypassed = false;


enum BiquadFilterType {
	Gain, LowPass, HighPass, /*BandPass, AllPass,*/ Peaking
};
enum BWQType {
	Q, BW, S
};

class BiquadFilter {
	protected:
		sample_t a0, a1, a2, b0, b1, b2;
		sample_t xn1, xn2, yn1, yn2;
	public:
		sample_t f0, gain_db, bwq;
		BiquadFilterType filter_type;
		BWQType bwq_type;
		BiquadFilter() {
			xn1 = xn2 = yn1 = yn2 = 0;
		}
		void update() {
			sample_t a = std::pow(10.0, gain_db/40.0);
			if (filter_type==Gain) {
				b0 = a;
				a0 = 1.0;
				a1 = a2 = b1 = b2 = 0.0;
				return;
			}
			sample_t w0 = 2.0*M_PI*f0/samplerate;
			sample_t sinw0 = std::sin(w0);
			sample_t cosw0 = std::cos(w0);
			sample_t alpha;
			if (bwq_type==Q) {
				alpha = sinw0/(2.0*bwq);
			} else if (bwq_type==BW) {
				alpha = sinw0*std::sinh(M_LN2/2.0*bwq*w0/std::sin(w0));
			} else if (bwq_type==S) {
				alpha = sinw0 * std::sqrt((a+1.0/a)*(1/bwq-1)+2) / 2.0;
			}
			if (filter_type==LowPass) {
				b1 = 1.0 - cosw0;
				b0 = b2 = b1/2.0;
				a0 = 1.0 + alpha;
				a1 = -2.0*cosw0;
				a2 = 1.0 - alpha;
			} else if (filter_type==HighPass) {
				b0 = b2 = (1.0 + cosw0)/2;
				b1 = -(1.0 + cosw0);
				a0 = 1.0 + alpha;
				a1 = -2.0*cos(w0);
				a2 = 1.0 - alpha;
			} else if (filter_type==Peaking) {
				b0 = 1.0 + alpha*a;
				b1 = -2.0*cosw0;
				b2 = 1.0 - alpha*a;
				a0 = 1.0 + alpha/a;
				a1 = -2.0*cosw0;
				a2 = 1.0 - alpha/a;
			} else if (filter_type==Gain) {
			}
		};
		void processBuffer(unsigned int period_size, sample_buffer_t buff) {
			for (unsigned int i=0; i<period_size; i++) {
				sample_t xn = buff[i]; // copy
				sample_t &yn = buff[i]; // reference; do not copy
				yn = (b0*xn + b1*xn1 + b2*xn2 - a1*yn1 - a2*yn2) / a0;
				xn2 = xn1;
				xn1 = xn;
				yn2 = yn1;
				yn1 = yn;
			}
		};
};

const size_t CHANNELS_MAX = 32;
const size_t FILTERS_MAX = 32;
const size_t CONFIG_LINE_MAX = 1024;

typedef struct {
	BiquadFilter* filters[FILTERS_MAX];
	size_t filters_count = 0;
	jack_port_t* input_port = NULL;
	jack_port_t* output_port = NULL;
} ChannelData;

jack_client_t* client = NULL;

typedef struct {
	size_t channels_count = 0;
	ChannelData* chd[CHANNELS_MAX];
} DSP;

DSP* dsp_prev = NULL;
DSP* dsp_curr = NULL;
DSP* dsp_next = NULL;

char* conf_fn;

int process(jack_nframes_t nframes, void* data) {
	if (dsp_next!=NULL) {
		dsp_prev = dsp_curr;
		dsp_curr = dsp_next;
		dsp_next = NULL;
	}
	for (int chn=0; chn<dsp_curr->channels_count; chn++) {
		ChannelData* ch = dsp_curr->chd[chn];
		sample_buffer_t buff_in;
		sample_buffer_t buff_out;
		buff_in = (sample_buffer_t)jack_port_get_buffer(ch->input_port, nframes);
		buff_out = (sample_buffer_t)jack_port_get_buffer(ch->output_port, nframes);
		memcpy(buff_out, buff_in, sizeof(sample_t)*nframes);
		// remove denormals etc...
		uint32_t* tsmp = (uint32_t*)buff_out;
		for (int i=0; i<nframes; i++) {
			uint32_t sample = *tsmp;
			uint32_t smpexp = sample & 0x7F800000;
			int smp_nan = smpexp < 0x7F800000;
			int smp_den = smpexp > 0;
			*tsmp = sample * (smp_nan & smp_den);
			tsmp++;
		}
		// start filtering:
		for (int fin=0; fin<ch->filters_count; fin++) {
			BiquadFilter* fi = ch->filters[fin];
			fi->processBuffer(nframes, buff_out);
		}
	}
	return 0;
}
int loadconf() {
	debug("Loading configuration from %s\n", conf_fn);
	FILE* fcfg = fopen(conf_fn, "r");
	if (fcfg==NULL) {
		fprintf(stderr, "Unable to open config file.\n");
		return EXIT_FAILURE;
	}
	DSP* nextdsp = new DSP;
	char client_name[CONFIG_LINE_MAX];
	char confline[CONFIG_LINE_MAX];
	jack_status_t status;
	while(!feof(fcfg)) {
		if (fgets(confline, CONFIG_LINE_MAX-1, fcfg)==NULL) break;
		if ((confline[0]=='\r') || (confline[0]=='\n') || (confline[0]=='\0')) continue;
		char cmd[CONFIG_LINE_MAX];
		int readvals = sscanf(confline, "%s ", cmd);
		if ((readvals<1) || (cmd[0]=='#')) { //comment
			debug("Empty line or comment\n");
			continue;
		}
		else if (strcmp(cmd, "client")==0) {
			if (client!=NULL) continue;
			sscanf(confline, "%*s %s", client_name);
			client = jack_client_open(client_name, JackNullOption, &status, NULL);
			if (client==NULL) {
				fprintf(stderr, "Failed to connect to JACK.\n");
				return EXIT_FAILURE;
			}
			samplerate = jack_get_sample_rate(client);
			debug("Created JACK client %s. Samplerate %d.\n", client_name, samplerate);
		}
		else if (strcmp(cmd, "channels")==0) {
			int chancount;
			sscanf(confline, "%*s %d", &chancount);
			int currchannels = 0;
			if (dsp_curr!=NULL) currchannels = dsp_curr->channels_count;
			/*if (chancount>currchannels) {
				for (int i=currchannels; i<chancount; i++) {
					nextdsp->chd[i] = new ChannelData;
					nextdsp->chd[i]->filters_count = 0;
					char portname[32];
					sprintf(portname, "input_%d", i+1);
					nextdsp->chd[i]->input_port = jack_port_register(client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
					sprintf(portname, "output_%d", i+1);
					nextdsp->chd[i]->output_port = jack_port_register(client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
				}
			}*/
			for (int i=0; i<chancount; i++) {
				nextdsp->chd[i] = new ChannelData;
				nextdsp->chd[i]->filters_count = 0;
				if (i>=currchannels) {
					char portname[32];
					sprintf(portname, "input_%d", i+1);
					nextdsp->chd[i]->input_port = jack_port_register(client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
					sprintf(portname, "output_%d", i+1);
					nextdsp->chd[i]->output_port = jack_port_register(client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
				} else {
					if (dsp_curr==NULL) debug("BUG: attempting to copy ports but dsp_curr==NULL !!!\n");
					nextdsp->chd[i]->input_port  = dsp_curr->chd[i]->input_port;
					nextdsp->chd[i]->output_port = dsp_curr->chd[i]->output_port;
				}
			}
			nextdsp->channels_count = chancount;
			fprintf(stderr, "Prepared ChannelData for %d channels.\n", chancount);
		}
		else if (strcmp(cmd, "f")==0) {
			char ftype_s[CONFIG_LINE_MAX];
			char chandef[CONFIG_LINE_MAX];
			float freq;
			float bw;
			float gain;
			sscanf(confline, "%*s %s %s %f %f %f", chandef, ftype_s, &freq, &bw, &gain);
			BiquadFilterType ftype;
			if ((strcmp(ftype_s, "peaking")==0) || (strcmp(ftype_s, "peak")==0) || (strcmp(ftype_s, "pk")==0)) ftype = Peaking;
			else if ((strcmp(ftype_s, "lp")==0) || (strcmp(ftype_s, "hc")==0)) ftype = LowPass;
			else if ((strcmp(ftype_s, "hp")==0) || (strcmp(ftype_s, "lc")==0)) ftype = HighPass;
			else if ((strcmp(ftype_s, "g")==0) || (strcmp(ftype_s, "gain")==0)) ftype = Gain;
			else {
				fprintf(stderr, "Invalid filter type: %s", ftype_s);
				return EXIT_FAILURE;
			}

			// extract channels from chandef:
			char chann_s[CONFIG_LINE_MAX];
			char* chann_c = chann_s;
			for (char* chandef_c = chandef; ; chandef_c++) {
				debug("[%c] ", *chandef_c);
				if ((*chandef_c>='0') && (*chandef_c<='9')) {
					*chann_c = *chandef_c;
					chann_c++;
				} else if ((*chandef_c==',') || (*chandef_c=='\0')) {
					*chann_c = '\0';
					chann_c = chann_s;
					int channum = atoi(chann_s) - 1;
					if ((channum<0) || (channum>=nextdsp->channels_count)) {
						fprintf(stderr, "Invalid channel number: %d\n", channum+1);
						return EXIT_FAILURE;
					}

					BiquadFilter* fi = new BiquadFilter();
					fi->f0 = freq;
					fi->gain_db = gain;
					fi->bwq_type = BW;
					fi->bwq = bw/60.0;
					fi->filter_type = ftype;
					fi->update();
					nextdsp->chd[channum]->filters[nextdsp->chd[channum]->filters_count] = fi;
					nextdsp->chd[channum]->filters_count++;
					debug("Added filter to channel %d.\n", channum+1);
					if (*chandef_c=='\0') break;
				} else {
					fprintf(stderr, "Invalid character \"%c\" in ports specification \"%s\"\n", *chandef_c, chandef);
					return EXIT_FAILURE;
				}
			}
		} else {
			fprintf(stderr, "Unknown command in config file: %s\n", cmd);
			return EXIT_FAILURE;
		}
	}
	fclose(fcfg);
	dsp_next = nextdsp;
	return EXIT_SUCCESS;
}



void term(int signum) {
	jack_deactivate(client);
	exit(signum);
}
void reload_config(int signum) {
	loadconf();
}



int main(int argc, char** argv) {
	if (argc!=2) {
		fprintf(stderr, "Usage: %s config_file\n", argv[0]);
		return EXIT_FAILURE;
	}
	conf_fn = argv[1];
	if (loadconf()!=0) {
		fprintf(stderr, "Initial config loading failed!\n");
		return EXIT_FAILURE;
	}
	if (client==NULL) {
		fprintf(stderr, "JACK client not ready, check your config!\n");
		return EXIT_FAILURE;
	}
	jack_set_process_callback(client, process, NULL);
	/*for (int i=0;i<channels_count;i++) {
		debug("Channel %d: %d filters.\n", i+1, chd[i]->filters_count);
	}*/
	signal(SIGINT, term);
	signal(SIGTERM, term);
	signal(SIGUSR1, reload_config);
	signal(SIGHUP, reload_config);
	jack_activate(client);
	debug("JACK activated.\n");
	while(true) { // collect garbage in main thread
		sleep(1);
		if ((dsp_next==NULL) && (dsp_prev!=NULL)) {
			if (dsp_prev->channels_count>dsp_curr->channels_count) {
				for (int i=dsp_curr->channels_count; i<dsp_prev->channels_count; i++) {
					jack_port_unregister(client, dsp_prev->chd[i]->input_port);
					jack_port_unregister(client, dsp_prev->chd[i]->output_port);
				}
			}
			for (int i=0; i<dsp_prev->channels_count; i++) {
				for (int j=0; j<dsp_prev->chd[i]->filters_count; j++) {
					delete dsp_prev->chd[i]->filters[j];
				}
				delete dsp_prev->chd[i];
			}

			delete dsp_prev;
			dsp_prev = NULL;
		}
	}
	return 0;
}

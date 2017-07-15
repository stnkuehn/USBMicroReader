/*
    This tool creates CSV files with dB values. It can read sound data from
    a USB-mciro or from a wav-file.

    It is written to run on Linux. It is only tested with Ubuntu and Debian.

    Copyright (C) 2015  Steffen Kühn / steffen.kuehn@em-sys-dev.de

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

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <rfftw.h>
#include <sndfile.h>

#define DEFAULT_INPUT_FILE "-"
#define DEFAULT_OUTPUT_DIR "."
#define OUTPUT_MARKER "out"
#define DEFAULT_READ_COMMAND "/usr/bin/arecord -r 8000 -f S16_LE -D hw:1,0"

static char* output_dir = DEFAULT_OUTPUT_DIR;
static int samplerate = 0;
static int bitspersample = 0;
static int maxfreq = 100;
static int avg_int_in_sec = 60;
static gboolean in_db = FALSE;
static char* read_command = DEFAULT_READ_COMMAND;
static gboolean wav = FALSE;
static SNDFILE* wavfile = 0;
static SF_INFO sfinfo = {0};

typedef struct
{
	char ChunkID [4];
	int32_t ChunkSize;
	char Format [4];
	char Subchunk1ID [4];
	int32_t Subchunk1Size;
	int16_t AudioFormat;
	int16_t NumChannels;
	int32_t SampleRate;
	int32_t ByteRate;
	int16_t BlockAlign;
	int16_t BitsPerSample;
	int32_t Subchunk2ID;
	int32_t Subchunk2Size;
} t_RIFF_header;

static GOptionEntry entries[] =
{
	{
		"output-directory", 'd', 0, G_OPTION_ARG_FILENAME, &output_dir,
		"output dir, default: " DEFAULT_OUTPUT_DIR, NULL
	},
	{
		"mf", 'm', 0, G_OPTION_ARG_INT, &maxfreq,
		"max. frequency in Hz", NULL
	},
	{
		"ai", 'a', 0, G_OPTION_ARG_INT, &avg_int_in_sec,
		"averaging interval in seconds", NULL
	},
	{
		"command", 'c', 0, G_OPTION_ARG_STRING, &read_command,
		"read command: " DEFAULT_READ_COMMAND, NULL
	},
	{
		"db", 'l', 0, G_OPTION_ARG_NONE, &in_db,
		"output in dB", NULL
	},
	{
		"wav", 'w', 0, G_OPTION_ARG_NONE, &wav,
		"output wav-file too", NULL
	},
	{ NULL}
};

static void calc_power_spectrum(fftw_real* in, int N, fftw_real* power_spectrum)
{
	fftw_real out[N];
	rfftw_plan p;
	int k;

	p = rfftw_create_plan(N, FFTW_REAL_TO_COMPLEX, FFTW_ESTIMATE);

	rfftw_one(p, in, out);
	power_spectrum[0] = out[0] * out[0]; // DC component

	for (k = 1; k < (N + 1) / 2; ++k) // (k < N/2 rounded up)
	{
		power_spectrum[k] = out[k] * out[k] + out[N - k] * out[N - k];
	}

	if (N % 2 == 0) // N is even
	{
		power_spectrum[N / 2] = out[N / 2] * out[N / 2]; // Nyquist freq.
	}

	rfftw_destroy_plan(p);
}

static gboolean cheak_riff_header(t_RIFF_header* header)
{
	if (strncmp(header->ChunkID, "RIFF", 4)) return FALSE;

	if (strncmp(header->Format, "WAVE", 4)) return FALSE;

	if (strncmp(header->Subchunk1ID, "fmt ", 4)) return FALSE;

	if (header->Subchunk1Size != 16) return FALSE;

	if (header->AudioFormat != 1)
	{
		printf("ERROR: compressed wave file format unsupported\n");
		return FALSE;
	}

	if (header->NumChannels != 1)
	{
		printf("ERROR: only MONO supported\n");
		return FALSE;
	}

	samplerate = header->SampleRate;
	bitspersample = header->BitsPerSample;

	if ((bitspersample != 8) && (bitspersample != 16))
	{
		printf("ERROR: only 8 or 16 bit samples supported\n");
		return FALSE;
	}

	return TRUE;
}

static gdouble todB(gdouble v)
{
	return 10.0 * log10(v);
}

static gboolean does_file_exist(char* name)
{
	gboolean exist = FALSE;
	FILE* ofp = fopen(name, "r");

	if (ofp != NULL)
	{
		exist = TRUE;
		fclose(ofp);
	}

	return exist;
}

// The function tests if the file already exists and returns immediately if it
// is the case. Otherwise it creates a new file and writes a head line into it.
// Returns FALSE in the error case.
static gboolean file_prepare(char* name)
{
	// check if the output file already exist
	char* open_mode = "w";

	if (does_file_exist(name)) open_mode = "a";

	// create output file
	FILE* ofp = fopen(name, open_mode);

	if (!ofp)
	{
		printf("ERROR: could not open/create output file: %s\n", name);
		return FALSE;
	}

	// create header
	if (strcmp(open_mode, "a"))
	{
		fprintf(ofp, "timestamp");

		for (int i = 1; i < maxfreq; i++)
		{
			fprintf(ofp, ",%i Hz", i);
		}

		fprintf(ofp, "\n");
	}

	fclose(ofp);

	return TRUE;
}

static void close_wav(void)
{
	if (wavfile != 0)
	{
		sf_close(wavfile);
		wavfile = 0;
	}
}

static void write_wav(double* data, long size)
{
	if (wav == FALSE) return;

	time_t rawtime;
	time(&rawtime);
	struct tm* ti = localtime(&rawtime);

	// create file name
	char* filename = NULL;
	filename = g_strdup_printf("%s/%4.4i-%2.2i-%2.2i_%s.wav", output_dir,
							   ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday, OUTPUT_MARKER);

	if (!does_file_exist(filename))
	{
		// close old file
		close_wav();

		// file does not exist. create it and open it
		sfinfo.channels = 1;
		sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_DOUBLE;
		sfinfo.samplerate = samplerate;
		wavfile = sf_open(filename, SFM_WRITE, &sfinfo);
	}

	if (wavfile == 0)
	{
		// file exists but is not open
		wavfile = sf_open(filename, SFM_RDWR, &sfinfo);
		sf_seek(wavfile, 0, SEEK_END);
	}

	if (wavfile != 0)
	{
		sf_write_double(wavfile, data, size);
	}
}

static gboolean mainloop(FILE* fp)
{
	t_RIFF_header header;
	int32_t n = 0;

	n += fread(&header, 1, sizeof(t_RIFF_header), fp);

	if (n != sizeof(t_RIFF_header))
	{
		printf("ERROR: Read error\n");
		return FALSE;
	}

	if (!cheak_riff_header(&header))
	{
		printf("ERROR: invalid file format\n");
		return FALSE;
	}

	if (maxfreq > samplerate / 2) maxfreq = samplerate / 2;

	n = 0;
	int bytespersec = (bitspersample / 8) * samplerate;
	guchar buffer[bytespersec];

	fftw_real power_spectrum [avg_int_in_sec][samplerate / 2 + 1];
	int aind = 0;

	while (!feof(fp))
	{
		n += fread(buffer, 1, sizeof(buffer), fp);

		if (ferror(fp))
		{
			printf("ERROR: Read error\n");
			return FALSE;
		}

		fftw_real realbuf [samplerate];

		for (int i = 0; i < samplerate; i++)
		{
			if (bitspersample == 8)
			{
				realbuf[i] = buffer[i];
			}
			else
			{
				int32_t hb = buffer[2 * i + 1];
				int32_t lb = buffer[2 * i];
				int16_t v = (int16_t)(hb * (1 << 8) + lb);
				realbuf[i] = v;
			}

			realbuf[i] /= (1 << (bitspersample - 1));
		}

		write_wav(realbuf, samplerate);

		fftw_real* ps = power_spectrum[aind++];
		calc_power_spectrum(realbuf, samplerate, ps);

		if (aind == avg_int_in_sec)
		{
			aind = 0;

			time_t rawtime;
			struct tm * ti;

			time(&rawtime);
			ti = localtime(&rawtime);

			char* filename = NULL;
			filename = g_strdup_printf("%s/%4.4i-%2.2i-%2.2i_%s.csv",
									   output_dir, ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
									   OUTPUT_MARKER);

			if (!file_prepare(filename)) return FALSE;

			FILE* ofp = fopen(filename, "a");

			if (ofp == NULL)
			{
				printf("ERROR: could not reopen output file: %s\n", filename);
				return FALSE;
			}

			g_free(filename);

			fprintf(ofp, "%4.4i-%2.2i-%2.2i %2.2i:%2.2i:%2.2i",
					ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
					ti->tm_hour, ti->tm_min, ti->tm_sec);

			for (int k = 1; k < maxfreq; k++)
			{
				float v = 0;

				for (int j = 0; j < avg_int_in_sec; j++)
				{
					v += power_spectrum[j][k];
				}

				v /= avg_int_in_sec;

				if (!in_db)
				{
					fprintf(ofp, ",%.3f", v);
				}
				else
				{
					fprintf(ofp, ",%.3f", todB(v));
				}
			}

			fprintf(ofp, "\n");
			fclose(ofp);
		}
	}

	return TRUE;
}

static gboolean controlloop(void)
{
	gchar** rcstr = g_strsplit(read_command, " ", 0);
	GPid child_pid;
	gint standard_output;
	GError* error = NULL;
	gboolean res;

	// start input process
	for (;;)
	{
		res = g_spawn_async_with_pipes(
				  NULL, rcstr, NULL, G_SPAWN_DEFAULT, NULL, NULL,
				  &child_pid, NULL, &standard_output, NULL, &error);

		if (!res)
		{
			printf("ERROR: could not spawn child process: %s\n",
				   error->message ? error->message : "");
			g_error_free(error);
			error = NULL;
		}
		else
		{
			printf("child process launched. process id is %i\n", child_pid);
			FILE* fp = fdopen(standard_output, "r");
			mainloop(fp);
			fclose(fp);
		}

		// wait five seconds for the next run
		usleep(5000000);
	}

	g_free(rcstr);

	close_wav();

	return TRUE;
}

int main(int argc, char *argv[])
{
	gboolean res = TRUE;
	GOptionContext *context = NULL;

	context = g_option_context_new("");
	char* help = g_strdup_printf(
					 "for reading from a micro: %s -c '/usr/bin/arecord -r 8000 -f S16_LE -D hw:1,0'\n"
					 "for reading from a file:  %s -c '/bin/cat test.wav'\n",
					 argv[0], argv[0]);
	g_option_context_set_summary(context, help);
	g_free(help);
	g_option_context_add_main_entries(context, entries, NULL);

	if (!g_option_context_parse(context, &argc, &argv, NULL))
	{
		res = FALSE;
		printf("ERROR: invalid options\n");
	}
	else if (!controlloop())
	{
		res = FALSE;
		printf("ERROR: problem in mainloop\n");
	}

	// return code 0 means everything was OK
	return (!res);
}

#include <vector>
#include <string>
#include <cmath>
#include <cassert>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>

#include <FL/Fl.H>
#include <FL/Enumerations.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/fl_draw.H>

#include <sndfile.h>
#include <portaudio.h>

static const char *APP_NAME = "subttl";
static const int REFRESH_RATE = 25; // Hz

struct Segment {
private:
  std::string text_;
public:
  int start;
  Segment(int s, const char *t="")
    : start(s), text_(t)
  {}
  const char* text() const { return text_.c_str(); }
  void text(const char *t) {
    text_ = t;
    // strip out empty lines
    size_t pos;
    while ((pos = text_.find("\n\n")) != std::string::npos) {
      text_.erase(pos+1,1);
    }
  }
};

struct TimeOffset {
  int hours;
  int minutes;
  int seconds;
  int ms;
};

class SampleBuffer {
  float *samples_;
  int nframes_;
  int nchannels_;
  int samplerate_;
  int cursor_;
  std::vector<Segment> segments_;
  int curseg_;
public:
  SampleBuffer(float *samples, int nframes, int nchannels, int samplerate)
    : samples_(samples),
      nframes_(nframes),
      nchannels_(nchannels),
      samplerate_(samplerate),
      cursor_(0),
      curseg_(0)
  {
    segments_.push_back(Segment(0));
    segments_.push_back(Segment(nframes)); // sentinel
  }
  int nframes() { return nframes_; }
  int nchannels() { return nchannels_; }
  int cursor() { return cursor_; }
  const Segment& curseg() { return segments_[curseg_]; }
  const Segment& nextseg() { return segments_[curseg_+1]; }
  void jumpToSegStart() {
    cursor_ = curseg().start;
  }
  void updateSegmentText(const char *t) {
    segments_[curseg_].text(t);
  }
  void findSegmentsInRange(int start, int end, std::vector<int> &result) {
    for (int i=0; i<segments_.size()-1; i++) {
      int st = segments_[i].start;
      if (st >= start && st < end) {
        result.push_back(st);
      }
    }
  }
  void addSegment() {
    if (cursor_ > curseg().start and cursor_ < nframes_) {
      segments_.insert(segments_.begin()+curseg_+1, Segment(cursor_));
      update();
    }
  }
  void joinSegment() {
    if (cursor_ < nframes_ && curseg_ > 0) {
      std::string savedText = curseg().text();
      segments_.erase(segments_.begin()+curseg_);
      curseg_--;
      std::string t = curseg().text();
      t.append(savedText);
      updateSegmentText(t.c_str());
    }
  }
  void skipSegments(int count) {
    // we never use the last segment (sentinel)
    while ( (count > 0) && (curseg_ < segments_.size()-2) ) {
      curseg_++;
      count--;
    }
    while (count < 0 && curseg_ > 0) {
      curseg_--;
      count++;
    }
    cursor_ = curseg().start;
  }
  void update() {
    while ( (curseg_ < segments_.size()-2) && (cursor_ >= nextseg().start) ) {
      curseg_++;
    }
    while ( (curseg_ > 0) && (cursor_ < curseg().start) ) {
      curseg_--;
    }
  }
  void skipSeconds(float seconds) {
    int samplesToSkip = seconds*samplerate_;
    cursor_ += samplesToSkip;
    if (cursor_ > nframes_) {
      cursor_ = nframes_-1;
    }
    if (cursor_ < 0) {
      cursor_ = 0;
    }
    update();
  }
  bool copyFramesForPlayback(float *target, int count, bool stopPlayingAtSegmentEnd) {
    float *src = samples_+cursor_*nchannels_;
    int end = stopPlayingAtSegmentEnd ? nextseg().start : nframes_;
    while (cursor_ < end && count > 0) {
      for (int i=0; i<nchannels_; i++) {
        *target++ = *src++;
      }
      ++cursor_;
      --count;
    }
    while (count > 0) {
      for (int i=0; i<nchannels_; i++) {
        *target++ = 0;
      }
      --count;
    }
    bool shouldStop = false;
    if (cursor_ == end) {
      cursor_ = end-1;
      if (stopPlayingAtSegmentEnd) {
        shouldStop = true;
      }
    }
    update();
    return shouldStop;
  }
  float peak(int offset, int spp) {
    float *start = samples_+offset*nchannels_;
    float *end = samples_+nframes_*nchannels_;
    float pk = 0;
    for (float *sptr = start; sptr < std::min(start+spp,end); sptr += nchannels_) {
      float max = 0;
      for (int j=0; j<nchannels_; j++) {
        float cur = std::abs(sptr[j]);
        if (cur > max) max = cur;
      }
      if (max > pk) pk = max;
    }
    return pk;
  }
  void calcTimeOffset(int offset, TimeOffset *to) {
    float seconds = offset / samplerate_;
    to->seconds = (int) seconds;
    to->ms = (seconds - (float) to->seconds) * 1000.0;
    to->minutes = to->seconds / 60;
    to->seconds %= 60;
    to->hours = to->minutes / 60;
    to->minutes %= 60;
  }
  void saveSegments(std::string &srtpath) {
    std::string tmppath = srtpath + ".tmp";
    FILE *f = fopen(tmppath.c_str(), "w");
    if (f) {
      TimeOffset start, end;
      for (int i=0; i<segments_.size()-1; i++) {
        fprintf(f, "%d\n", i+1);
        calcTimeOffset(segments_[i].start, &start);
        calcTimeOffset(segments_[i+1].start, &end);
        fprintf(f, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\n",
                start.hours, start.minutes, start.seconds, start.ms,
                end.hours, end.minutes, end.seconds, end.ms);
        fputs(segments_[i].text(), f);
        fprintf(f, "\n");
      }
      fclose(f);
      if (rename(tmppath.c_str(), srtpath.c_str()) != 0) {
        Fl::warning("cannot save subtitles to %s: error when trying to replace with temp file\n", srtpath.c_str());
      }
      else {
        printf("subtitles successfully saved to %s\n", srtpath.c_str());
      }
    }
    else {
      Fl::warning("cannot create temporary file: %s\n", srtpath.c_str());
    }
  }
  void loadSegments(std::string &srtpath) {
    regex_t re_seq_number;
    regex_t re_start_end;
    regmatch_t matches[9];
    assert(regcomp(&re_seq_number, "^[0-9]+\n$", REG_NOSUB|REG_EXTENDED)==0);
    assert(regcomp(&re_start_end, "^([0-9]+):([0-9]+):([0-9]+),([0-9]+)[[:space:]]+-+>[[:space:]]+([0-9]+):([0-9]+):([0-9]+),([0-9]+)\n$", REG_EXTENDED)==0);
    const int LINESIZE = 256;
    char line[LINESIZE];
    int nline = 0;
    int nseq = 0;
    FILE *f = fopen(srtpath.c_str(), "r");
    if (f) {
      while (1) {
	// consume empty lines
	while (! feof(f)) {
	  if (fgets(line, LINESIZE, f)) {
	    ++nline;
	    // read until first non-empty line
	    if (strcmp(line,"\n")!=0) break;
	  }
	}
	if (feof(f)) break;
	// the first non-empty line must be the next sequence number
	++nseq;
	if (regexec(&re_seq_number, line, 0, 0, 0)!=0) {
	  Fl::fatal("error parsing SRT file %s: there should be a sequence number on line %d", srtpath.c_str(), nline);
	}
	if (nseq != atoi(line)) {
	  Fl::fatal("error parsing SRT file %s: bad sequence number on line %d, expected: %d, found: %d", srtpath.c_str(), nline, nseq, atoi(line));
	}
	// the second line must be the timeinfo (HH:MM:SS,fff --> HH:MM:SS,fff)
	//
	// (fff is milliseconds)
	if (! fgets(line, LINESIZE, f)) {
	  Fl::fatal("error parsing SRT file: %s", srtpath.c_str());
	}
	++nline;
	if (regexec(&re_start_end, line, 9, matches, 0)!=0) {
	  Fl::fatal("error parsing SRT file %s: invalid timecode on line %d", srtpath.c_str(), nline);
	}
	// make all matched numbers in `line' separate strings
	for (int i=1; i<=8; i++) {
	  *(line+matches[i].rm_eo) = 0;
	}
	TimeOffset from;
	from.hours   = atoi(line+matches[1].rm_so);
	from.minutes = atoi(line+matches[2].rm_so);
	from.seconds = atoi(line+matches[3].rm_so);
	from.ms      = atoi(line+matches[4].rm_so);
	TimeOffset to;
	to.hours     = atoi(line+matches[5].rm_so);
	to.minutes   = atoi(line+matches[6].rm_so);
	to.seconds   = atoi(line+matches[7].rm_so);
	to.ms        = atoi(line+matches[8].rm_so);
	// the next lines provide the text of the segment
	std::string text;
	while (fgets(line, LINESIZE, f)) {
	  ++nline;
	  // read up to the first empty line
	  if (strcmp(line,"\n")==0) break;
	  text.append(line);
	}
	cursor_ = ((float)from.hours*60*60
		   + (float)from.minutes*60
		   + (float)from.seconds
		   + ((float)from.ms/1000.0)) * (float)samplerate_;
	// don't add the first segment, it's already there
	if (cursor_ > 0) {
	  addSegment();
	}
	updateSegmentText(text.c_str());
      }
      fclose(f);
    }
    else {
      Fl::warning("cannot open SRT file for reading: %s\n", srtpath.c_str());
    }
    regfree(&re_start_end);
    regfree(&re_seq_number);
    cursor_ = 0;
    curseg_ = 0;
  }
};

class WaveWidget : public Fl_Widget {
  SampleBuffer *sb_;
  int spp_;
public:
  WaveWidget(int x, int y, int w, int h, SampleBuffer *sb)
    : Fl_Widget(x,y,w,h,0), sb_(sb), spp_(1000) {}
  void draw() {
    fl_color(FL_BACKGROUND_COLOR);
    fl_rectf(x(),y(),w(),h());
    int wh = w()/2;
    int offset = sb_->cursor()-spp_*wh;
    int cursegStart = sb_->curseg().start;
    int nextsegStart = sb_->nextseg().start;
    float hh = h()/2;
    float ymid = y()+hh;
    int xoffs = 0;
    std::vector<int> segStarts;
    sb_->findSegmentsInRange(offset, offset+spp_*w(), segStarts);
    int segStartIdx = 0;
    while (xoffs < w()) {
      fl_color(0x40,0x40,0x40);
      if (offset >= cursegStart && offset < nextsegStart) {
        fl_color(FL_BLACK);
      }
      if (offset >= 0 && offset+spp_ <= sb_->nframes()) {
        float v = sb_->peak(offset, spp_);
        fl_line(x()+xoffs, ymid-v*hh, x()+xoffs, ymid+v*hh);
      }
      if (segStartIdx < segStarts.size() && segStarts[segStartIdx] >= offset && segStarts[segStartIdx] < (offset+spp_)) {
        fl_color(FL_BLACK);
        static char dashes[3] = { 8,8,0 };
        fl_line_style(FL_DASH, 0, dashes);
        fl_line(x()+xoffs,y(),x()+xoffs,y()+h());
        fl_line_style(0);
        segStartIdx++;
      }
      offset += spp_;
      xoffs++;
    }
    fl_color(FL_BLUE);
    fl_line(x()+wh,ymid-hh/2,x()+wh,ymid+hh/2);
  }
  int handle(int event) {
    switch (event) {
      /* enable WaveWidget to take focus so that the editor can give
         it up */
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
    }
    return Fl_Widget::handle(event);
  }
};

void cbRedrawWidget(void *data) {
  Fl_Widget *w = (Fl_Widget*) data;
  w->redraw();
  Fl::repeat_timeout(1.0/REFRESH_RATE, cbRedrawWidget, w);
}

class MainWindow : public Fl_Double_Window {
  std::string path_;
  std::string srtpath_;
  SampleBuffer *sb_;
  bool playing_;
  bool playseg_;
  bool editing_;
  WaveWidget *waveWidget_;
  Fl_Text_Editor *editor_;

public:
  MainWindow(int w, int h, char *path, SampleBuffer *sb)
    : Fl_Double_Window(w, h),
      path_(path),
      srtpath_(path_+".srt"),
      sb_(sb),
      playing_(false),
      playseg_(false),
      editing_(false)
  {
    if (access(srtpath_.c_str(), F_OK)==0) {
      sb_->loadSegments(srtpath_);
    }
    label(APP_NAME);
    Fl_Tile *tile = new Fl_Tile(0,0,w,h);
    waveWidget_ = new WaveWidget(0,0,w,h/2,sb);
    editor_ = new Fl_Text_Editor(0,h/2,w,h/2);
    editor_->buffer(new Fl_Text_Buffer());
    tile->end();
    end();
    updateEditorText();
    Fl::add_timeout(1.0/REFRESH_RATE, cbRedrawWidget, waveWidget_);
  }
  void updateEditorText() {
    editor_->buffer()->text(sb_->curseg().text());
  }
  static void cbUpdateEditorText(void *data) {
    MainWindow *mw = (MainWindow*) data;
    mw->updateEditorText();
  }
  void copyFramesForPlayback(float *buf, int frameCount) {
    if (playing_) {
      Fl::lock();
      int savedStart = sb_->curseg().start;
      bool stopPlayingAtSegmentEnd = playseg_;
      bool shouldStop = sb_->copyFramesForPlayback(buf, frameCount, stopPlayingAtSegmentEnd);
      if (shouldStop) {
        playing_ = false;
      }
      /* if we switched to a different segment during playback, update
         the text in the editor */
      if (savedStart != sb_->curseg().start) {
        Fl::awake(cbUpdateEditorText, this);
      }
      Fl::unlock();
    }
    else {
      for (int i=0; i<frameCount*sb_->nchannels(); i++) {
        buf[i] = 0;
      }
    }
  }
  int handleKey(int keycode) {
    switch (keycode) {
    case ' ':
      sb_->jumpToSegStart();
      playing_ = true;
      playseg_ = true;
      break;
    case 'p':
      playing_ = not playing_;
      playseg_ = false;
      break;
    case 'm':
      sb_->addSegment();
      updateEditorText();
      break;
    case 'j':
      if (Fl::event_shift()) {
        sb_->joinSegment();
        updateEditorText();
        break;
      }
      else return 0;
    case ',':
      sb_->skipSegments(-1);
      updateEditorText();
      break;
    case '.':
      sb_->skipSegments(1);
      updateEditorText();
      break;
    case 'e':
      playing_ = false;
      editing_ = true;
      editor_->set_visible_focus();
      editor_->take_focus();
      break;
    case 's':
      if (Fl::event_shift()) {
	sb_->saveSegments(srtpath_);
	break;
      }
    case FL_Escape:
      if (editing_) {
        editing_ = false;
        sb_->updateSegmentText(editor_->buffer()->text());
	// update the editor because the segment may have modified the
	// text (e.g. may have removed empty lines from it)
	updateEditorText();
        editor_->clear_visible_focus();
        waveWidget_->take_focus();
      }
      else if (playing_) {
        playing_ = false;
      }
      break;
    case 'q':
      if (Fl::event_shift()) {
        playing_ = false;
        hide();
        break;
      }
    case FL_Left:
      sb_->skipSeconds(-1);
      updateEditorText();
      break;
    case FL_Right:
      sb_->skipSeconds(1);
      updateEditorText();
      break;
    default:
      return 0;
    }
    return 1;
  }
  int handle(int event) {
    switch (event) {
      /* we need to handle FL_FOCUS so that we get keypresses */
    case FL_FOCUS:
    case FL_UNFOCUS:
      return 1;
    case FL_KEYDOWN:
      return handleKey(Fl::event_key());
      break;
    }
    return Fl_Double_Window::handle(event);
  }
};

int dummyArgParser(int argc, char** argv, int &i) {
  return 0;
}

int paStreamCallback(const void *input,
                     void *output,
                     unsigned long frameCount,
                     const PaStreamCallbackTimeInfo *timeInfo,
                     PaStreamCallbackFlags statusFlags,
                     void *userData)
{
  float *buf = (float*) output;
  MainWindow *mw = (MainWindow*) userData;
  mw->copyFramesForPlayback(buf, frameCount);
  return paContinue;
}

int main(int argc, char **argv) {
  int argPos;
  if (Fl::args(argc, argv, argPos, dummyArgParser)==0) {
    Fl::fatal("cannot parse command line arguments.\n");
  }
  if (argPos==argc) {
    Fl::error("Usage: %s <path-to-audio-file>\n", APP_NAME);
    exit(0);
  }
  char *path = argv[argPos];
  SF_INFO sfInfo;
  sfInfo.format = 0;
  SNDFILE *sndfile = sf_open(path, SFM_READ, &sfInfo);
  if (sndfile==NULL) {
    Fl::fatal("Cannot open %s: %s\n", path, sf_strerror(sndfile));
    exit(1);
  }
  if (sfInfo.frames == 0) {
    Fl::fatal("Soundfile has no frames!\n");
  }
  printf("Soundfile information: frames=%llu, samplerate=%d, channels=%d\n", (long long) sfInfo.frames, sfInfo.samplerate, sfInfo.channels);
  float *samples = (float*) malloc(sizeof(float)*sfInfo.frames*sfInfo.channels);
  if (! samples) {
    Fl::fatal("not enough memory!\n");
  }
  printf("Reading sample data into memory... ");
  sf_command(sndfile, SFC_SET_NORM_FLOAT, NULL, SF_TRUE);
  sf_count_t framesRead = sf_readf_float(sndfile, samples, sfInfo.frames);
  if (framesRead != sfInfo.frames) {
    Fl::fatal("could not read all samples into memory, exiting.\n");
  }
  printf("done.\n");
  SampleBuffer *sb = new SampleBuffer(samples, sfInfo.frames, sfInfo.channels, sfInfo.samplerate);
  MainWindow *window = new MainWindow(640, 480, path, sb);
  window->show(argc, argv);
  printf("Initializing PortAudio...\n");
  PaError pa_error = Pa_Initialize();
  if (pa_error != paNoError) {
    Fl::fatal("Error initializing PortAudio: %s\n", Pa_GetErrorText(pa_error));
  }
  PaDeviceIndex devIndex = 0;
  while (1) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(devIndex);
    if (! info) break;
    printf("device #%d: %s\n", devIndex, info->name);
    ++devIndex;
  }
  printf("default output device is #%d\n", Pa_GetDefaultOutputDevice());
  PaStreamParameters outparams;
  outparams.device = Pa_GetDefaultOutputDevice();
  outparams.channelCount = sfInfo.channels;
  outparams.sampleFormat = paFloat32;
  outparams.suggestedLatency = 0;
  outparams.hostApiSpecificStreamInfo = 0;
  PaStream *pa_stream;
  pa_error = Pa_OpenStream(&pa_stream, 0, &outparams, sfInfo.samplerate, 0, 0, paStreamCallback, window);
  if (pa_error != paNoError) {
    Fl::fatal("Pa_OpenDefaultStream() failed: %s\n", Pa_GetErrorText(pa_error));
  }
  pa_error = Pa_StartStream(pa_stream);
  if (pa_error != paNoError) {
    Fl::fatal("Pa_StartStream() failed: %s\n", Pa_GetErrorText(pa_error));
  }
  Fl::lock();
  int status = Fl::run();
  Pa_StopStream(pa_stream);
  Pa_CloseStream(pa_stream);
  delete sb;
  Pa_Terminate();
  free(samples);
  sf_close(sndfile);
  return status;
}

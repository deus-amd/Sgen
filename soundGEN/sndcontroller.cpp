#include "sndcontroller.h"

SndController *SndController::_self_controller = 0;


FMOD_RESULT F_CALLBACK pcmreadcallback(FMOD_SOUND *sound, void *data, unsigned int datalen)
{
    SndController::Instance()->fillBuffer(sound, data, datalen);
    return FMOD_OK;
}

FMOD_RESULT F_CALLBACK pcmsetposcallback(FMOD_SOUND *sound, int subsound, unsigned int position, FMOD_TIMEUNIT postype)
{
    /*
        This is useful if the user calls Sound::setPosition and you want to seek your data accordingly.
    */
    return FMOD_OK;
}

double base_play_sound(int i, unsigned int c, double t) {
    return SndController::Instance()->playSound(i, c, t);
}

SndController::SndController(QObject *parent) :
    QObject(parent)
{
    baseSoundList = new SoundList(this);
    loop = new QEventLoop();
    timer = new QTimer();
    analyzer = new SndAnalyzer();
    analyzer->setTop_harmonic(1);
    all_functions_loaded = false;
    channels_count = 0;
    frequency = 0;
    oldParseHash = "";
    sound_functions = "";
    is_stopping = false;
    is_running = false;
    process_mode = SndPlay;

    unsigned int            version;
    /*
        Create a System object and initialize.
    */
    result = FMOD::System_Create(&system);
    ERRCHECK(result);

    result = system->getVersion(&version);
    ERRCHECK(result);

    if (version < FMOD_VERSION)
    {
        printf("Error!  You are using an old version of FMOD %08x.  This program requires %08x\n", version, FMOD_VERSION);
        exit(-1);
    }

    result = system->init(32, FMOD_INIT_NORMAL, 0);
    ERRCHECK(result);

    process_thread = new QThread();
    QObject::connect(process_thread,SIGNAL(started()),this,SLOT(process_sound()));
    QObject::connect(this,SIGNAL(finished()),process_thread,SLOT(terminate()));
    moveToThread(process_thread);
    loop->moveToThread(process_thread);
    timer->moveToThread(process_thread);
    #if !defined(ANDROID) && !defined(__ANDROID__)
    timer->setTimerType(Qt::PreciseTimer);
    #endif
    connect(timer, SIGNAL(timeout()), this, SLOT(updateTimer()));

    memset(&createsoundexinfo_sound, 0, sizeof(FMOD_CREATESOUNDEXINFO));
    createsoundexinfo_sound.cbsize            = sizeof(FMOD_CREATESOUNDEXINFO); /* required. */
    createsoundexinfo_sound.format            = FMOD_SOUND_FORMAT_PCM32;        /* Data format of sound. */
    createsoundexinfo_gen = createsoundexinfo_sound;
    createsoundexinfo_gen.pcmreadcallback   = pcmreadcallback;                  /* User callback for reading. */
    createsoundexinfo_gen.pcmsetposcallback = pcmsetposcallback;                /* User callback for seeking. */

    setChannelsCount(2);
    setFrequency(44100.0);
    resetParams();
}

SndController::~SndController()
{
    setChannelsCount(0);

    process_thread->quit();

    result = system->close();
    ERRCHECK(result);
    result = system->release();
    ERRCHECK(result);

    delete loop;
    delete timer;
    delete baseSoundList;
    delete analyzer;
    delete process_thread;
}

SndController *SndController::Instance()
{
    if(!_self_controller)
    {
        _self_controller = new SndController();
    }
    return _self_controller;
}

bool SndController::DeleteInstance()
{
    if(_self_controller)
    {
        delete _self_controller;
        _self_controller = 0;
        return true;
    }
    return false;
}

double SndController::playSound(int index, unsigned int channel, double t)
{
    return baseSoundList->playSound(index, channel, t);
}

void SndController::setChannelsCount(unsigned int count)
{
    if (count<channels.size()) {
        while (count>=0 && count<channels.size()) {
            delete channels.last();
            channels.remove(channels.size()-1);
        }
    } else if (count>channels.size()) {
        while (count>channels.size()) {
            GenSoundChannelInfo *info = new GenSoundChannelInfo();
            info->amp = 1;
            info->freq = 500;
            info->channel_fct = 0;
            info->function_text = "sin(k*t)";
            info->k = info->freq*2.0*M_PI;
            info->fr = 0;
            info->ar = 0;
            channels.append(info);
        }
    }

    channels_count = count;

    createsoundexinfo_sound.length            = ((unsigned int) frequency) * channels_count * sizeof(qint32); /* Length of PCM data in bytes of whole song (for Sound::getLength) */
    createsoundexinfo_sound.numchannels       = channels_count;                                               /* Number of channels in the sound. */
    createsoundexinfo_gen.length = createsoundexinfo_sound.length;
    createsoundexinfo_gen.numchannels = createsoundexinfo_sound.numchannels;
}

unsigned int SndController::getChannelsCount()
{
    return channels_count;
}

void SndController::fillBuffer(FMOD_SOUND *sound, void *data, unsigned int datalen)
{
    unsigned int  count;
    qint32 *buffer = (qint32*)data;
    qint32 max_val = std::numeric_limits<qint32>::max();

    datalen = datalen/(channels_count*sizeof(qint32));

    if (all_functions_loaded)
    {
        for(unsigned int i=0; i<channels_count; i++)
        {
            double curr = 0;

            for (count=0; count<datalen; count++)
            {
                curr = getResult(i, t+count/frequency);
                buffer[count*channels_count + i] = (qint32)(curr * max_val);
            }
        }

        t += datalen/frequency;
    }
}

QString SndController::getCurrentParseHash()
{
    QCryptographicHash hash(QCryptographicHash::Sha1);

    hash.addData(EnvironmentInfo::getConfigsPath().toLatin1());
    #if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
        hash.addData(QString::number(QFile::exists(EnvironmentInfo::getConfigsPath()+"/efr/main.dll")).toLatin1());
    #else
        hash.addData(QString::number(QFile::exists(EnvironmentInfo::getConfigsPath()+"/efr/main.so")).toLatin1());
    #endif
    hash.addData(QString::number(channels_count).toLatin1());
    hash.addData(QString::number(lib.isLoaded()).toLatin1());
    hash.addData(sound_functions.toLatin1());
    hash.addData(text_functions.toLatin1());
    for(int i=0;i<channels_count;i++) {
        hash.addData(channels.at(i)->function_text.toLatin1());
    }

    return hash.result().toHex();
}

bool SndController::checkHash(bool emptyCheck)
{
    QString parseHash = "";

    if (emptyCheck || !oldParseHash.isEmpty()) {
        parseHash = getCurrentParseHash();
        if (!emptyCheck || oldParseHash.isEmpty()) {
            qDebug() << tr("Hash is: ") << parseHash;
        }
    }

    if (!emptyCheck && !oldParseHash.isEmpty() && !parseHash.isEmpty() && oldParseHash==parseHash) {
        qDebug() << tr("Hashes equals - no need to parse functions");
        return true;
    }

    if (!parseHash.isEmpty()) {
        oldParseHash = parseHash;
    }

    return false;
}

bool SndController::parseFunctions()
{
    if (checkHash(false)) return true;

    QString error = "";
    bool add_base_functions = false;
    all_functions_loaded = false;
    unsigned int i;

    if (lib.isLoaded()) {
        lib.unload();
    }

    QDir dir(EnvironmentInfo::getConfigsPath());
    dir.mkdir("efr");

    if (QFile::exists(EnvironmentInfo::getConfigsPath()+"/base_functions.h")) {
        if (QFile::exists(EnvironmentInfo::getConfigsPath()+"/efr/base_functions.cpp"))
        {
            QFile::remove(EnvironmentInfo::getConfigsPath()+"/efr/base_functions.cpp");
        }
        QFile::copy(EnvironmentInfo::getConfigsPath()+"/base_functions.cpp", EnvironmentInfo::getConfigsPath()+"/efr/base_functions.cpp");
        if (QFile::exists(EnvironmentInfo::getConfigsPath()+"/efr/base_functions.h"))
        {
            QFile::remove(EnvironmentInfo::getConfigsPath()+"/efr/base_functions.h");
        }
        QFile::copy(EnvironmentInfo::getConfigsPath()+"/base_functions.h", EnvironmentInfo::getConfigsPath()+"/efr/base_functions.h");
        add_base_functions = true;
    }

    #if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
        QString main_file_name = "main.cpp";
        QString spec_func_pref = "extern \"C\" __declspec(dllexport)";
        QString spec_namespace = "using namespace std;\n";
        QString lib_file = "main.dll";
    #else
        QString main_file_name = "main.c";
        QString spec_func_pref = "extern \"C\"";
        QString spec_namespace = "";
        QString lib_file = "main.so";
    #endif

    if (QFile::exists(EnvironmentInfo::getConfigsPath()+"/efr/"+lib_file) && !QFile::remove(EnvironmentInfo::getConfigsPath()+"/efr/"+lib_file))
    {
        qDebug() <<  tr("Can't remove %filename%").replace("%filename%", lib_file) << endl;
        #if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
            int lib_n = 0;
            do {
                lib_file = "main_"+QString::number(lib_n)+".dll";
                lib_n++;
            } while (QFile::exists(EnvironmentInfo::getConfigsPath()+"/efr/"+lib_file) && !QFile::remove(EnvironmentInfo::getConfigsPath()+"/efr/"+lib_file));
        #endif
    }

    QProcess* pConsoleProc = new QProcess;

    QFile file(EnvironmentInfo::getConfigsPath()+"/efr/main.h");
    file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream out(&file);
    out << "#include <math.h>\n";
    out << "#include <string.h>\n";
    out << "#include <stdio.h>\n";
    if (add_base_functions) out << "#include \"base_functions.h\"\n";
    out << "typedef double (*PlaySoundFunction) (int,unsigned int,double);\n";
    for(i=0;i<channels_count;i++) {
        out << spec_func_pref << " double sound_func_"+QString::number(i)+"(double t, double k, double f, PlaySoundFunction __bFunction);\n";
    }
    file.close();

    QFile file2(EnvironmentInfo::getConfigsPath()+"/efr/"+main_file_name);
    file2.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream out2(&file2);
    out2 << "#include \"main.h\"\n";
    out2 << spec_namespace;
    out2 << "\nPlaySoundFunction BaseSoundFunction;\n";
    out2 << "\n" + sound_functions + "\n";
    out2 << "\n" + text_functions + "\n";
    for(i=0;i<channels_count;i++) {
        out2 << spec_func_pref << "double sound_func_"+QString::number(i)+"(double t, double k, double f, PlaySoundFunction __bFunction) { BaseSoundFunction=__bFunction; return (double) ("+channels.at(i)->function_text+"); };\n";
    }
    out2 << "int main() {return 0;};\n";
    file2.close();

    #if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
        QString tcmd;
        QString vcdir = EnvironmentInfo::getVCPath();
        if (!vcdir.isEmpty())
        {
            pConsoleProc->setWorkingDirectory(vcdir);
            pConsoleProc->start("vcvarsall.bat x86_amd64");
            pConsoleProc->waitForFinished();
            qDebug() << pConsoleProc->workingDirectory() << endl;
        }

        pConsoleProc->setWorkingDirectory(EnvironmentInfo::getConfigsPath()+"/efr");
        qDebug() << pConsoleProc->workingDirectory() << endl;

        if (add_base_functions) {
            pConsoleProc->start("cl.exe /c /EHsc base_functions.cpp");
            pConsoleProc->waitForFinished();
            qDebug() <<  pConsoleProc->readAll() << endl;
            pConsoleProc->start("lib base_functions.obj");
            pConsoleProc->waitForFinished();
            qDebug() <<  pConsoleProc->readAll() << endl;
            tcmd = "cl.exe /LD main.cpp /DLL /link base_functions.lib /OUT:" + lib_file;
        } else {
            tcmd = "cl.exe /LD main.cpp /link /DLL /OUT:" + lib_file;
        }
    #else
        QFile file3(EnvironmentInfo::getConfigsPath()+"/efr/Makefile");
        file3.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        QTextStream out3(&file3);

        out3 << "CC = g++\n";
        out3 << "MODULES= main.o base_functions.o\n";
        out3 << "OBJECTS=\n";
        out3 << "RCLOBJECTS= main.c main.h base_functions.cpp base_functions.h\n";
        out3 << "all: "+lib_file+"\n";
        out3 << lib_file+":$(MODULES)\n";
        out3 << "	$(CC) -shared $(MODULES) -o "+lib_file+"\n";
        out3 << "base_functions.o: $(RCLOBJECTS)\n";
        out3 << "	g++ -m64 -Wall -fPIC -c base_functions.cpp -o base_functions.o\n";
        out3 << "main.o: $(RCLOBJECTS)\n";
        out3 << "	g++ -m64 -Wall -fPIC -c main.c -o main.o\n";
        out3 << "clean:\n";
        out3 << "	rm -f *.o\n";
        out3 << "	rm -f "+lib_file+"\n";
        file3.close();

        QString tcmd = "make -C \""+EnvironmentInfo::getConfigsPath()+"/efr\" -f Makefile";
    #endif
    qDebug() <<  tcmd << endl;

    pConsoleProc->start(tcmd, QProcess::ReadOnly);
    if(pConsoleProc->waitForFinished()==true)
    {
       QByteArray b = pConsoleProc->readAllStandardError();
       #if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(_WIN64)
           error = pConsoleProc->readAll();
           qDebug() <<  error << endl;
           if (error.indexOf("fatal error", 0, Qt::CaseInsensitive)<0 && error.indexOf("error c", 0, Qt::CaseInsensitive)<0) {
               error = "";
           }
       #else
           error = QString(b);
       #endif
       qDebug() <<  error << endl;
    }
    pConsoleProc->close();
    delete pConsoleProc;

    if (error.isEmpty()) {
        lib.setFileName(EnvironmentInfo::getConfigsPath()+"/efr/"+lib_file);
        all_functions_loaded = true;
        for(i=0;i<channels_count;i++) {
            channels.at(i)->channel_fct = (GenSoundFunction)(lib.resolve(qPrintable("sound_func_"+QString::number(i))));
            all_functions_loaded = all_functions_loaded && channels.at(i)->channel_fct;
        }
        checkHash(true);
    }

    if (!all_functions_loaded) {
        oldParseHash = "";
        for(i=0;i<channels_count;i++) {
            channels.at(i)->channel_fct = 0;
        }
    }

    return all_functions_loaded;
}

double SndController::getResult(unsigned int channel, double current_t)
{
    GenSoundChannelInfo *info = channels.at(channel);
    return info->amp * info->channel_fct(current_t, info->k, info->freq, base_play_sound);
}

void SndController::resetParams()
{
    for(unsigned int i=0; i<channels_count; i++) {
        GenSoundChannelInfo *info = channels.at(i);
        info->amp = 1;
        info->freq = 500;
        info->k = info->freq*2.0*M_PI;
        info->ar = 0;
        info->fr = 0;
        info->function_text = "sin(k*t)";
    }
    t = 0.0;
    t_real = 0.0;
}

SoundList *SndController::getBaseSoundList() const
{
    return baseSoundList;
}

double SndController::getFrequency() const
{
    return frequency;
}

void SndController::setFrequency(double value)
{
    frequency = value;
    createsoundexinfo_sound.decodebuffersize  = (unsigned int) frequency;                                     /* Chunk size of stream update in samples.  This will be the amount of data passed to the user callback. */
    createsoundexinfo_sound.length            = ((unsigned int) frequency) * channels_count * sizeof(qint32); /* Length of PCM data in bytes of whole song (for Sound::getLength) */
    createsoundexinfo_sound.defaultfrequency  = (unsigned int) frequency;                                     /* Default playback rate of sound. */
    createsoundexinfo_gen.decodebuffersize = createsoundexinfo_sound.decodebuffersize;
    createsoundexinfo_gen.length = createsoundexinfo_sound.length;
    createsoundexinfo_gen.defaultfrequency = createsoundexinfo_sound.defaultfrequency;
}


void SndController::writeWavHeader(FILE *file, FMOD::Sound *sound, int length)
{
    int             channels, bits;
    float           rate;

    if (!sound)
    {
        return;
    }

    fseek(file, 0, SEEK_SET);

    sound->getFormat  (0, 0, &channels, &bits);
    sound->getDefaults(&rate, 0, 0, 0);

    {
        #if defined(WIN32) || defined(_WIN64) || defined(__WATCOMC__) || defined(_WIN32) || defined(__WIN32__)
        #pragma pack(push, 1)
        #endif

        /*
            WAV Structures
        */
        typedef struct
        {
            signed char id[4];
            int 		size;
        } RiffChunk;

        struct
        {
            RiffChunk       chunk           __PACKED;
            unsigned short	wFormatTag      __PACKED;    /* format type  */
            unsigned short	nChannels       __PACKED;    /* number of channels (i.e. mono, stereo...)  */
            unsigned int	nSamplesPerSec  __PACKED;    /* sample rate  */
            unsigned int	nAvgBytesPerSec __PACKED;    /* for buffer estimation  */
            unsigned short	nBlockAlign     __PACKED;    /* block size of data  */
            unsigned short	wBitsPerSample  __PACKED;    /* number of bits per sample of mono data */
        } __PACKED FmtChunk  = { {{'f','m','t',' '}, sizeof(FmtChunk) - sizeof(RiffChunk) }, 1, channels, (int)rate, (int)rate * channels * bits / 8, 1 * channels * bits / 8, bits };

        struct
        {
            RiffChunk   chunk;
        } DataChunk = { {{'d','a','t','a'}, length } };

        struct
        {
            RiffChunk   chunk;
            signed char rifftype[4];
        } WavHeader = { {{'R','I','F','F'}, sizeof(FmtChunk) + sizeof(RiffChunk) + length }, {'W','A','V','E'} };

        #if defined(WIN32) || defined(_WIN64) || defined(__WATCOMC__) || defined(_WIN32) || defined(__WIN32__)
        #pragma pack(pop)
        #endif

        /*
            Write out the WAV header.
        */
        fwrite(&WavHeader, sizeof(WavHeader), 1, file);
        fwrite(&FmtChunk, sizeof(FmtChunk), 1, file);
        fwrite(&DataChunk, sizeof(DataChunk), 1, file);
    }
}

void SndController::export_cycle(FMOD::Sound *sound)
{
    emit export_status(0);

    FILE *mainfile = fopen(qPrintable(export_filename), "wb");

    if (mainfile) {
        unsigned int datalength = 0;
        writeWavHeader(mainfile, sound, datalength);

        unsigned int sec_buff_size = ((unsigned int) frequency) * channels_count * sizeof(qint32);
        quint8 *buf = new quint8[sec_buff_size];
        t = 0.0;
        for(int second = 0; second<export_max_t; second++) {
            fillBuffer(0, buf, sec_buff_size);
            datalength += fwrite(buf, 1, sec_buff_size, mainfile);
            emit export_status(round(100.0*second/export_max_t));
        }
        delete buf;

        if (datalength) {
            writeWavHeader(mainfile, sound, datalength);
        }

        fclose(mainfile);
        qDebug() << tr("Sound object written to file");

        emit export_status(100);
    } else {
        emit write_message(tr("Error write to file: %filename%").replace("%filename%",export_filename));
    }
}

void SndController::play_cycle(FMOD::Sound *sound)
{
    FMOD::Channel          *channel = 0;
    unsigned int i;

    /*
        Play the sound.
    */
    result = system->playSound(FMOD_CHANNEL_FREE, sound, 0, &channel);
    ERRCHECK(result);

    timer->start(500);
    is_running = true;
    /*
        Main loop.
    */
    do
    {
        system->update();

        emit cycle_start();

        if (channel)
        {
            unsigned int ms;
            unsigned int lenms;
            bool         playing;
            bool         paused;

            channel->isPlaying(&playing);
            if ((result != FMOD_OK) && (result != FMOD_ERR_INVALID_HANDLE) && (result != FMOD_ERR_CHANNEL_STOLEN))
            {
                ERRCHECK(result);
            }

            result = channel->getPaused(&paused);
            if ((result != FMOD_OK) && (result != FMOD_ERR_INVALID_HANDLE) && (result != FMOD_ERR_CHANNEL_STOLEN))
            {
                ERRCHECK(result);
            }

            result = channel->getPosition(&ms, FMOD_TIMEUNIT_MS);
            if ((result != FMOD_OK) && (result != FMOD_ERR_INVALID_HANDLE) && (result != FMOD_ERR_CHANNEL_STOLEN))
            {
                ERRCHECK(result);
            }

            result = sound->getLength(&lenms, FMOD_TIMEUNIT_MS);
            if ((result != FMOD_OK) && (result != FMOD_ERR_INVALID_HANDLE) && (result != FMOD_ERR_CHANNEL_STOLEN))
            {
                ERRCHECK(result);
            }
        }

        for(i=0; i<channels.size(); i++) {
            analyzer->function_fft_top_only(getChannelFunction(i), base_play_sound, t - 0.5, t + 0.5, channels.at(i)->freq, 1*frequency);
            channels.at(i)->fr = analyzer->getInstFrequency();
            channels.at(i)->ar = channels.at(i)->amp * analyzer->getInstAmp();
        }

        QTimer::singleShot(1000, loop, SLOT(quit()));
        loop->exec();
    } while (!is_stopping);
    timer->stop();

    if (channel) {
        channel->setPaused(true);
    }
}

void SndController::process_sound()
{
    FMOD::Sound            *sound;
    FMOD_MODE               mode = FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL | FMOD_SOFTWARE;
    QTextStream             console(stdout);
    GenSoundChannelInfo    *info;
    bool parsed;

    t = t_real = 0.0;
    is_stopping = false;
    emit write_message(tr("Initialization..."));

    console << tr("Starting with:") << " " << endl;
    for(unsigned int i=0; i<channels_count; i++) {
        info = channels.at(i);
        console << tr("Function %num%:").replace("%num%",QString::number(i)) << " " << info->function_text << endl;
        console << tr("Amp %num%:").replace("%num%",QString::number(i)) << " " << info->amp << endl;
        console << tr("Freq %num%:").replace("%num%",QString::number(i)) << " " << info->freq << endl;
    }

    sound_functions = baseSoundList->getFunctionsText();
    console << tr("Sounds:") << "[" << sound_functions << "]" << endl;

    parsed = parseFunctions();

    if (!parsed) {
        emit write_message(tr("Error in functions!"));
        process_thread->quit();
        return;
    }

    if (process_mode == SndPlay) emit started();

    mode |= FMOD_CREATESTREAM;

    result = system->createSound(0, mode, &createsoundexinfo_gen, &sound);
    ERRCHECK(result);

    emit write_message(tr("Playing"));

    switch (process_mode) {
        case SndPlay:
            play_cycle(sound);
        break;
        case SndExport:
            export_cycle(sound);
        break;
    }

    printf("\n");

    /*
        Shut down
    */
    result = sound->release();
    ERRCHECK(result);

    is_stopping = false;
    is_running = false;

    emit write_message(tr("Stopped"));
    if (process_mode == SndExport) emit export_finished();
    emit finished();
    if (process_mode == SndExport) {
        emit stopped();
        process_thread->quit();
    }

    return;
}

void SndController::updateTimer()
{
    t_real_ms_unixtime = QDateTime::currentMSecsSinceEpoch();
    t_real += timer->interval() / 1000.0;
    emit write_message(tr("Time: %sec%s").replace("%sec%",QString::number(t_real, 'f', 1)));
}

void SndController::run() {
    process_mode = SndPlay;
    process_thread->start();
    while (process_thread->isFinished()) {}
}

void SndController::stop() {
    is_stopping = true;
    loop->exit();
    process_thread->quit();
    while (!process_thread->isFinished()) {};
    emit stopped();
}

void SndController::run_export(int seconds, QString filename) {
    process_mode = SndExport;
    export_max_t = seconds;
    export_filename = filename;
    process_thread->start();
    while (process_thread->isFinished()) {}
}

void SndController::setFunctionsStr(QString new_f) {
    text_functions = new_f;
}

void SndController::setFunctionStr(unsigned int channel, QString new_text)
{
    channels.at(channel)->function_text = new_text;
}

void SndController::setAmp(unsigned int channel, double new_amp)
{
    channels.at(channel)->amp = new_amp;
}

void SndController::setFreq(unsigned int channel, double new_freq)
{
    channels.at(channel)->freq = new_freq;
    channels.at(channel)->k = new_freq*2.0*M_PI;
}

double SndController::getInstFreq(unsigned int channel)
{
    return channels.at(channel)->fr;
}

double SndController::getInstAmp(unsigned int channel)
{
    return channels.at(channel)->ar;
}

double SndController::getT()
{
    double rt;
    rt = QDateTime::currentMSecsSinceEpoch() - t_real_ms_unixtime;
    if (!timer->isActive() || rt>2*timer->interval()) {
        rt = 0;
    } else {
        rt = rt / 1000;
    }
    return t_real + rt;
}

GenSoundFunction SndController::getChannelFunction(unsigned int channel)
{
    if (lib.isLoaded() && channel>=0 && channel<channels.size())
        return channels.at(channel)->channel_fct;
    else
        return 0;
}

FMOD::System *SndController::getFmodSystem()
{
    return system;
}

FMOD_CREATESOUNDEXINFO SndController::getFmodSoundCreateInfo()
{
    return createsoundexinfo_sound;
}

bool SndController::running()
{
    return is_running;
}

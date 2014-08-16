#ifndef SOUNDLIST_H
#define SOUNDLIST_H

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <QString>
#include <QVector>
#include <QDebug>
#include <fmod.hpp>
#include <fmod_errors.h>

struct GenSoundRecord {
    QString sound_file;
    QString sound_function;
    FMOD::Sound *base_sound;
    unsigned int soundLenPcmBytes;
    qint16 *pcmData;
};

class SoundList
{
private:
    QVector<GenSoundRecord*> baseSoundsList;
    FMOD::System* system;
    void ERRCHECK(FMOD_RESULT result);
    void ConvertSoundBuffer(void *buf, int length, int bits_count, int channels_count, float frequency, void **outbuf, unsigned int *outlength);
public:
    SoundList();
    void clearSounds();
    void setSystem(FMOD::System* new_system);
    void AddSound(QString new_file, QString new_function);
    QString getFunctionsText();
    double playSound(int index, unsigned int channels, double t);
    void InitSounds();
};

#endif // SOUNDLIST_H

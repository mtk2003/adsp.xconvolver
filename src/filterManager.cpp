/*
*      Copyright (C) 2005-2014 Team KODI
*      http://KODI.org
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with KODI; see the file COPYING.  If not, see
*  <http://www.gnu.org/licenses/>.
*
*/


#include <template/include/ADSPAddonMain.h>
#include <string>

using namespace std;
using namespace ADDON;

#include "filterManager.h"
#include <LibXConvolverCore/include/LXC_Core.h>
#include <LibXConvolverCore/LXC_fftHandles/LXC_fftHandles.h>
#include <LibXConvolverUtils/LXC_Signal/LXC_WavLoader.h>
#include <LibXConvolverUtils/LXC_Signal/LXC_Resampler.h>

// Helper functions
string DSPChannelToString(AE_DSP_CHANNEL Channel);
string DSPChannelFlagToString(AE_DSP_CHANNEL_PRESENT ChannelFlag);
string channelToFilterFile(string Channel);

// Initialize static element
CFilterManager* CFilterManager::m_Instance = NULL;

CFilterManager::CFilterManager()
{
  for (uint ii = 0; ii < AE_DSP_CH_MAX; ii++)
  {
    m_Filters[ii].filter = NULL;
    m_Filters[ii].maxFilterLength = 0;
    m_Filters[ii].sampleFrequency = 0;
  }
}

CFilterManager::~CFilterManager()
{
  for (uint ii = 0; ii < AE_DSP_CH_MAX; ii++)
  {
    if (m_Filters[ii].filter)
    {
      delete [] m_Filters[ii].filter;
      m_Filters[ii].filter = NULL;
    }

    m_Filters[ii].maxFilterLength = 0;
    m_Filters[ii].sampleFrequency = 0;
  }
}

CFilterManager *CFilterManager::Get()
{
  static CFilterManagerGuard g;           // Memory cleaning
  if (!m_Instance)
  {
    m_Instance = new CFilterManager();
  }

  return m_Instance;
}


bool CFilterManager::LoadFilters()
{
  unsigned long channelFlags = 0;//ADSP->GetCurrentPresentSinkChannelFlags();

  for (uint ii = 0; ii < AE_DSP_CH_MAX; ii++)
  {
    if (channelFlags & 1 << ii)
    {
      string channelName = DSPChannelFlagToString((AE_DSP_CHANNEL_PRESENT)(1 << ii));
      m_Filters[ii].filter = NULL;
      m_Filters[ii].maxFilterLength = 0;
      m_Filters[ii].sampleFrequency = 0;

      // ToDo: Load filters from wave files
      LXC_WavStruct filterFile;
      filterFile.samples = NULL;
      unsigned int loadedSamples = LXC_CWavLoader::openWavFile(channelToFilterFile(channelName), &filterFile);
      if (loadedSamples > 0)
      {
        float *tempArray = NULL;
        m_Filters[ii].maxFilterLength = filterFile.maxSamples;
        m_Filters[ii].sampleFrequency = filterFile.sampleFrequency;

        if (filterFile.maxChannels > 1)
        {
          LXC_CWavLoader::reorderChannels(&filterFile);
          // ToDo: show some warning, because only one channel is used
          
          // only the first channel will be used
          tempArray = new float[m_Filters[ii].maxFilterLength];
          if (!tempArray)
          {
            // ToDo: show some error message
          }
        }

        // store samples into internal buffers
        if (tempArray)
        {
          memcpy(tempArray, filterFile.samples, sizeof(float)*m_Filters[ii].maxFilterLength);
          m_Filters[ii].filter = tempArray;
        }
        else
        {
          m_Filters[ii].filter = filterFile.samples;
        }
      }
    }
    else
    {
      // set parameters, so that XConvolver will bypass this channel
      if (m_Filters[ii].filter)
      {
        delete[] m_Filters[ii].filter;
      }

      m_Filters[ii].filter = NULL;
      m_Filters[ii].maxFilterLength = 0;
      m_Filters[ii].sampleFrequency = 0;
    }
  }

  return true;
}

STREAM_FILTER *CFilterManager::CreateStreamFilter(uint SampleFrequency, uint MaxInputFrameLength, LXC_FFT_MODULE fftModule, LXC_OPTIMIZATION_MODULE OptModule/* = LXC_OPT_NATIVE*/)
{
  STREAM_FILTER *streamFilter = new STREAM_FILTER;
  if (!streamFilter)
    return NULL;

  // reset buffers
  for (uint ii = 0; ii < AE_DSP_CH_MAX; ii++)
  {
    streamFilter->filters[ii].buffer = NULL;
    streamFilter->filters[ii].maxFilterLength = 0;
    streamFilter->filters[ii].maxFilterPartLength = 0;
    streamFilter->filters[ii].maxFilterPartLength_NonZero = 0;
    streamFilter->filters[ii].maxFilterParts = 0;
    streamFilter->filters[ii].sampleFrequency = 0;
  }

  streamFilter->lxcConfig.LXC_module = OptModule;

  // calculate next power of two
  // very important for fft length
  uint maxFreqFrameLength = 0;
  LXC_Core_checkPowerOfTwo(MaxInputFrameLength*2, &maxFreqFrameLength);

  // get LXC_fftModule
  LXC_ERROR_CODE err = LXC_get_fftHandle(&(streamFilter->fftHandle), fftModule, OptModule, maxFreqFrameLength, MaxInputFrameLength);
  if (err != LXC_NO_ERR)
  {
    // ToDo LXC error Code to String
    KODI->Log(LOG_ERROR, "Could not create stream filter. ToDo LXC Error fftModule!");
    DestroyStreamFilter(&streamFilter);
    return NULL;
  }

  // get LXC_OPTIMIZATION_MODULE callbacks
  err = LXC_Core_getModuleCallbacks(&(streamFilter->lxcCallbacks),
                                    &(streamFilter->bufferCallbacks),
                                    &(streamFilter->ringbufferCallbacks),
                                    OptModule);
  if (err != LXC_NO_ERR)
  {
    // ToDo LXC error Code to String
    KODI->Log(LOG_ERROR, "Could not get LXC_Callbacks.");
    DestroyStreamFilter(&streamFilter);
    return NULL;
  }

  // check filters for resampling
  LXC_CResampler *resampler = NULL;
  for (uint ii = 0; ii < AE_DSP_CH_MAX && !resampler; ii++)
  {
    if (m_Filters[ii].filter && m_Filters[ii].sampleFrequency != SampleFrequency)
    {
      resampler = new LXC_CResampler(1024, m_Filters[ii].sampleFrequency, SampleFrequency);
      if (!resampler)
      {
        KODI->Log(LOG_ERROR, "Could not create stream filter resampler.");
        CFilterManager::DestroyStreamFilter(&streamFilter);

        // ToDo throw some error
        return NULL;
      }
    }
  }

  for (uint ii = 0; ii < AE_DSP_CH_MAX; ii++)
  {
    if (m_Filters[ii].filter)
    {
      FILTER_SAMPLES tempFilter;
      tempFilter.filter = NULL;
     
      // Is resampling requiered?
      bool requireResampling = false;
      if (m_Filters[ii].sampleFrequency != SampleFrequency)
      { // perform filter samples resampling
        requireResampling = true;
        
        tempFilter.maxFilterLength = LXC_CResampler::CalcMaxOutputSamples(m_Filters[ii].maxFilterLength, m_Filters[ii].sampleFrequency, SampleFrequency);
        tempFilter.sampleFrequency = SampleFrequency;
        
        // create temp buffer
        tempFilter.filter = new float[tempFilter.maxFilterLength];
        if (!tempFilter.filter)
        {
          CFilterManager::DestroyStreamFilter(&streamFilter);

          if (resampler)
          {
            delete resampler;
            resampler = NULL;
          }

          // ToDo: show some error message
          return NULL;
        }

        // resample filter taps
        err = resampler->Resample(m_Filters[ii].filter, m_Filters[ii].maxFilterLength, m_Filters[ii].sampleFrequency, SampleFrequency, tempFilter.filter);
        if (err != LXC_NO_ERR)
        {
          if (err == LXC_ERR_RESAMPLER)
          {
            KODI->Log(LOG_ERROR, "%s: Resampler error: %s", __FUNCTION__, resampler->GetLastError().c_str());
          }

          CFilterManager::DestroyStreamFilter(&streamFilter);

          if (resampler)
          {
            delete resampler;
            resampler = NULL;
          }

          if (requireResampling)
          {
            delete[] tempFilter.filter;
            tempFilter.filter = NULL;
          }

          // ToDo: show some error message
          return NULL;
        }
      }
      else
      {
        tempFilter.filter = m_Filters[ii].filter;
        tempFilter.maxFilterLength = m_Filters[ii].maxFilterLength;
        tempFilter.sampleFrequency = m_Filters[ii].sampleFrequency;
      }

      if (!tempFilter.filter)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);

        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        // ToDo: show some error message
        return NULL;
      }

      // set filter parameters
      streamFilter->filters[ii].buffer = NULL;
      streamFilter->filters[ii].maxFilterLength = tempFilter.maxFilterLength;
      streamFilter->filters[ii].maxFilterPartLength = streamFilter->fftHandle.LXC_fftPlan.LXC_fftSize;
      streamFilter->filters[ii].maxFilterPartLength_NonZero = streamFilter->fftHandle.LXC_fftPlan.LXC_maxInputFrameLength;
      streamFilter->filters[ii].maxFilterParts = LXC_Core_calcMaxFilterParts(tempFilter.maxFilterLength, streamFilter->filters[ii].maxFilterPartLength_NonZero);
      streamFilter->filters[ii].sampleFrequency = tempFilter.sampleFrequency;
      if (streamFilter->filters[ii].maxFilterParts == 0)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        // ToDo: throw some error message
        return NULL;
      }

      // create buffer
      err = streamFilter->bufferCallbacks.LXC_Buffer_create(&streamFilter->filters[ii]);
      if (err != LXC_NO_ERR)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        // ToDo: throw some error message
        return NULL;
      }

      // Transform samples into frequency domain
      float *TempSamples = new float[streamFilter->fftHandle.LXC_fftPlan.LXC_fftSize];
      if (!TempSamples)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        // ToDo: throw some error message
        return NULL;
      }
      const uint arrayStep  = streamFilter->fftHandle.LXC_fftPlan.LXC_maxInputFrameLength;
      const uint maxParts   = streamFilter->filters[ii].maxFilterParts;
      const uint maxPartSize = streamFilter->fftHandle.LXC_fftPlan.LXC_fftSize;
      for (uint part = 0; part < maxParts -1; part++)
      {
        // copy samples to internal structure
        float *p = tempFilter.filter + part*arrayStep;
        for (uint jj = 0; jj < arrayStep; jj++)
          TempSamples[jj] = p[jj];

        // zero padding
        for (uint jj = arrayStep; jj < maxPartSize; jj++)
          TempSamples[jj] = 0.0f;

        err = streamFilter->fftHandle.LXC_fftCallbacks.LXC_fmtc_external_TO_fft(TempSamples, &(streamFilter->fftHandle.LXC_fftPlan), maxPartSize);
        if (err != LXC_NO_ERR)
        {
          CFilterManager::DestroyStreamFilter(&streamFilter);
          if (resampler)
          {
            delete resampler;
            resampler = NULL;
          }

          if (requireResampling)
          {
            delete[] tempFilter.filter;
            tempFilter.filter = NULL;
          }

          delete [] TempSamples;
          TempSamples = NULL;

          // ToDo: throw some error message
          return NULL;
        }

        err = streamFilter->fftHandle.LXC_fftCallbacks.LXC_fft(&(streamFilter->fftHandle.LXC_fftPlan));
        if (err != LXC_NO_ERR)
        {
          CFilterManager::DestroyStreamFilter(&streamFilter);
          if (resampler)
          {
            delete resampler;
            resampler = NULL;
          }

          if (requireResampling)
          {
            delete[] tempFilter.filter;
            tempFilter.filter = NULL;
          }

          delete[] TempSamples;
          TempSamples = NULL;

          // ToDo: throw some error message
          return NULL;
        }

        err = streamFilter->fftHandle.LXC_fftCallbacks.LXC_fmtc_fft_TO_internal(&(streamFilter->fftHandle.LXC_fftPlan),
                                                                        streamFilter->bufferCallbacks.LXC_Buffer_getPart(&(streamFilter->filters[ii]), part), 
                                                                        maxPartSize);
        if (err != LXC_NO_ERR)
        {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        delete[] TempSamples;
        TempSamples = NULL;

        // ToDo: throw some error message
        return NULL;
        }
      }      
      
      // copy last filter part
      uint lastSamples = streamFilter->filters[ii].maxFilterLength - arrayStep*(maxParts - 1);
      float *p = tempFilter.filter + arrayStep*(maxParts - 1);
      for (uint jj = 0; jj < lastSamples; jj++)
        TempSamples[jj] = p[jj];

      // zero padding
      for (uint jj = lastSamples; jj < maxPartSize; jj++)
        TempSamples[jj] = 0.0f;

      err = streamFilter->fftHandle.LXC_fftCallbacks.LXC_fmtc_external_TO_fft(TempSamples, &(streamFilter->fftHandle.LXC_fftPlan), maxPartSize);
      if (err != LXC_NO_ERR)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        delete[] TempSamples;
        TempSamples = NULL;

        // ToDo: throw some error message
        return NULL;
      }

      err = streamFilter->fftHandle.LXC_fftCallbacks.LXC_fft(&(streamFilter->fftHandle.LXC_fftPlan));
      if (err != LXC_NO_ERR)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        delete[] TempSamples;
        TempSamples = NULL;

        // ToDo: throw some error message
        return NULL;
      }

      err = streamFilter->fftHandle.LXC_fftCallbacks.LXC_fmtc_fft_TO_internal(&(streamFilter->fftHandle.LXC_fftPlan),
        streamFilter->bufferCallbacks.LXC_Buffer_getPart(&(streamFilter->filters[ii]), maxParts -1),
        maxPartSize);
      if (err != LXC_NO_ERR)
      {
        CFilterManager::DestroyStreamFilter(&streamFilter);
        if (resampler)
        {
          delete resampler;
          resampler = NULL;
        }

        if (requireResampling)
        {
          delete[] tempFilter.filter;
          tempFilter.filter = NULL;
        }

        delete[] TempSamples;
        TempSamples = NULL;

        // ToDo: throw some error message
        return NULL;
      }

      if (TempSamples)
      {
        delete[] TempSamples;
        TempSamples = NULL;
      }

      // if resampling was required delete buffer
      if (requireResampling)
      {
        delete[] tempFilter.filter;
        tempFilter.filter = NULL;
      }
    }
    else
    { // set channel to bypass mode
      streamFilter->filters[ii].buffer = NULL;
      streamFilter->filters[ii].maxFilterLength = 0;
      streamFilter->filters[ii].maxFilterPartLength = 0;
      streamFilter->filters[ii].maxFilterPartLength_NonZero = 0;
      streamFilter->filters[ii].maxFilterParts = 0;
      streamFilter->filters[ii].sampleFrequency = 0;
    }
  }

  // if resampling was required delete buffer
  if (resampler)
  {
    delete resampler;
  }

  return streamFilter;
}

void CFilterManager::DestroyStreamFilter(STREAM_FILTER **StreamFilter)
{
  if (*StreamFilter)
  {
    // destroy filters
    LXC_BUFFER_CALLBACKS *bufferCallbacks = &((*StreamFilter)->bufferCallbacks);
    for (uint ii = 0; ii < AE_DSP_CH_MAX; ii++)
    {
      if (bufferCallbacks->LXC_Buffer_destroy)
      {
        bufferCallbacks->LXC_Buffer_destroy(&((*StreamFilter)->filters[ii]));
      }
    }

    // destroy fft module
    if ((*StreamFilter)->fftHandle.LXC_fftCallbacks.LXC_destroy_fft)
    {
      (*StreamFilter)->fftHandle.LXC_fftCallbacks.LXC_destroy_fft(&((*StreamFilter)->fftHandle.LXC_fftPlan));
    }

    delete *StreamFilter;
    *StreamFilter = NULL;
  }
}

string DSPChannelFlagToString(AE_DSP_CHANNEL_PRESENT ChannelFlag)
{
  string channelStr = "AE_DSP_CH_INVALID";

  switch (ChannelFlag)
  {
    case AE_DSP_PRSNT_CH_FL:    channelStr = "AE_DSP_CH_FL";      break;
    case AE_DSP_PRSNT_CH_FR:    channelStr = "AE_DSP_CH_FR";      break;
    case AE_DSP_PRSNT_CH_FC:    channelStr = "AE_DSP_CH_FC";      break;
    case AE_DSP_PRSNT_CH_LFE:   channelStr = "AE_DSP_CH_LFE";     break;
    case AE_DSP_PRSNT_CH_BL:    channelStr = "AE_DSP_CH_BL";      break;
    case AE_DSP_PRSNT_CH_BR:    channelStr = "AE_DSP_CH_BR";      break;
    case AE_DSP_PRSNT_CH_FLOC:  channelStr = "AE_DSP_CH_FLOC";    break;
    case AE_DSP_PRSNT_CH_FROC:  channelStr = "AE_DSP_CH_FROC";    break;
    case AE_DSP_PRSNT_CH_BC:    channelStr = "AE_DSP_CH_BC";      break;
    case AE_DSP_PRSNT_CH_SL:    channelStr = "AE_DSP_CH_SL";      break;
    case AE_DSP_PRSNT_CH_SR:    channelStr = "AE_DSP_CH_SR";      break;
    case AE_DSP_PRSNT_CH_TFL:   channelStr = "AE_DSP_CH_TFL";     break;
    case AE_DSP_PRSNT_CH_TFR:   channelStr = "AE_DSP_CH_TFR";     break;
    case AE_DSP_PRSNT_CH_TFC:   channelStr = "AE_DSP_CH_TFC";     break;
    case AE_DSP_PRSNT_CH_TC:    channelStr = "AE_DSP_CH_TC";      break;
    case AE_DSP_PRSNT_CH_TBL:   channelStr = "AE_DSP_CH_TBL";     break;
    case AE_DSP_PRSNT_CH_TBR:   channelStr = "AE_DSP_CH_TBR";     break;
    case AE_DSP_PRSNT_CH_TBC:   channelStr = "AE_DSP_CH_TBC";     break;
    case AE_DSP_PRSNT_CH_BLOC:  channelStr = "AE_DSP_CH_BLOC";    break;
    case AE_DSP_PRSNT_CH_BROC:  channelStr = "AE_DSP_CH_BROC";    break;
    default:                    channelStr = "AE_DSP_CH_INVALID"; break;
  }

  return channelStr;
}

string DSPChannelToString(AE_DSP_CHANNEL Channel)
{
  string channelStr = "AE_DSP_CH_INVALID";

  switch (Channel)
  {
    case AE_DSP_CH_FL:    channelStr = "AE_DSP_CH_FL";      break;
    case AE_DSP_CH_FR:    channelStr = "AE_DSP_CH_FR";      break;
    case AE_DSP_CH_FC:    channelStr = "AE_DSP_CH_FC";      break;
    case AE_DSP_CH_LFE:   channelStr = "AE_DSP_CH_LFE";     break;
    case AE_DSP_CH_BL:    channelStr = "AE_DSP_CH_BL";      break;
    case AE_DSP_CH_BR:    channelStr = "AE_DSP_CH_BR";      break;
    case AE_DSP_CH_FLOC:  channelStr = "AE_DSP_CH_FLOC";    break;
    case AE_DSP_CH_FROC:  channelStr = "AE_DSP_CH_FROC";    break;
    case AE_DSP_CH_BC:    channelStr = "AE_DSP_CH_BC";      break;
    case AE_DSP_CH_SL:    channelStr = "AE_DSP_CH_SL";      break;
    case AE_DSP_CH_SR:    channelStr = "AE_DSP_CH_SR";      break;
    case AE_DSP_CH_TFL:   channelStr = "AE_DSP_CH_TFL";     break;
    case AE_DSP_CH_TFR:   channelStr = "AE_DSP_CH_TFR";     break;
    case AE_DSP_CH_TFC:   channelStr = "AE_DSP_CH_TFC";     break;
    case AE_DSP_CH_TC:    channelStr = "AE_DSP_CH_TC";      break;
    case AE_DSP_CH_TBL:   channelStr = "AE_DSP_CH_TBL";     break;
    case AE_DSP_CH_TBR:   channelStr = "AE_DSP_CH_TBR";     break;
    case AE_DSP_CH_TBC:   channelStr = "AE_DSP_CH_TBC";     break;
    case AE_DSP_CH_BLOC:  channelStr = "AE_DSP_CH_BLOC";    break;
    case AE_DSP_CH_BROC:  channelStr = "AE_DSP_CH_BROC";    break;
    default:              channelStr = "AE_DSP_CH_INVALID"; break;
  }

  return channelStr;
}

string channelToFilterFile(string Channel)
{
  string filterFile = g_strAddonPath;
  if (!(filterFile.at(filterFile.size() - 1) == '\\' ||
    filterFile.at(filterFile.size() - 1) == '/'))
  {
#ifdef TARGET_WINDOWS
    filterFile += "\\";
#else
    filterFile += "/";
#endif
  }

  filterFile += Channel + ".wav";

  return filterFile;
}


/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    endpoints.h

Abstract:

    Node and Pin numbers and other common definitions for simple audio sample.
--*/

#ifndef _SIMPLEAUDIOSAMPLE_ENDPOINTS_H_
#define _SIMPLEAUDIOSAMPLE_ENDPOINTS_H_

// Name Guid
// {0104947F-82AE-4291-A6F3-5E2DE1AD7DC2}
#define STATIC_NAME_SIMPLE_AUDIO_SAMPLE\
    0x104947f, 0x82ae, 0x4291, 0xa6, 0xf3, 0x5e, 0x2d, 0xe1, 0xad, 0x7d, 0xc2
DEFINE_GUIDSTRUCT("0104947F-82AE-4291-A6F3-5E2DE1AD7DC2", NAME_SIMPLE_AUDIO_SAMPLE);
#define NAME_SIMPLE_AUDIO_SAMPLE DEFINE_GUIDNAMED(NAME_SIMPLE_AUDIO_SAMPLE)

//----------------------------------------------------
// New defines for the capture endpoints.
//----------------------------------------------------

// Default pin instances.
#define MAX_INPUT_STREAMS           1       // Number of capture streams.

// Wave pins
enum 
{
    KSPIN_WAVE_BRIDGE = 0,
    KSPIN_WAVEIN_HOST,
};

// Wave Topology nodes.
enum 
{
    KSNODE_WAVE_ADC = 0
};

// Wave Topology nodes.
enum 
{
    KSNODE_WAVE_DAC = 0
};

// topology pins.
enum
{
    KSPIN_TOPO_MIC_ELEMENTS,
    KSPIN_TOPO_BRIDGE
};

// topology nodes.
enum
{
    KSNODE_TOPO_VOLUME,
    KSNODE_TOPO_MUTE,
    KSNODE_TOPO_PEAKMETER
};

// data format attribute range definitions.
static
KSATTRIBUTE PinDataRangeSignalProcessingModeAttribute =
{
    sizeof(KSATTRIBUTE),
    0,
    STATICGUIDOF(KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE),
};

static
PKSATTRIBUTE PinDataRangeAttributes[] =
{
    &PinDataRangeSignalProcessingModeAttribute,
};

static
KSATTRIBUTE_LIST PinDataRangeAttributeList =
{
    ARRAYSIZE(PinDataRangeAttributes),
    PinDataRangeAttributes,
};

#endif // _SIMPLEAUDIOSAMPLE_ENDPOINTS_H_

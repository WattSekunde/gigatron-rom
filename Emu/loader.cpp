#include <stdint.h>
#include <stdio.h>
#include <string>
#include <fstream>
#include <algorithm>

#include "cpu.h"
#include "editor.h"
#include "loader.h"
#include "timing.h"
#include "graphics.h"
#include "assembler.h"


namespace Loader
{
    enum LoaderState {FirstByte=0, MsgLength, LowAddress, HighAddress, Message, LastByte, ResetIN, NumLoaderStates};
    enum FrameState {Resync=0, Frame, Execute, NumFrameStates};


    bool _startUploading = false;
    bool _disableUploads = false;


    bool getStartUploading(void) {return _startUploading;}
    void setStartUploading(bool start) {_startUploading = start;}


    bool loadGt1File(const std::string& filename, Gt1File& gt1File)
    {
        std::ifstream infile(filename, std::ios::binary | std::ios::in);
        if(!infile.is_open())
        {
            fprintf(stderr, "Loader::loadGt1File() : failed to open '%s'\n", filename.c_str());
            return false;
        }

        int segmentCount = 1;
        for(;;)
        {
            // Read segment header
            Gt1Segment segment;
            infile.read((char *)&segment._hiAddress, SEGMENT_HEADER_SIZE);
            if(infile.eof() || infile.bad() || infile.fail())
            {
                fprintf(stderr, "Loader::loadGt1File() : bad header in segment %d of '%s'\n", segmentCount, filename.c_str());
                return false;
            }

            // Finished
            if(segment._hiAddress == 0x00  &&  infile.peek() == EOF)
            {
                // Segment header aligns with Gt1File terminator, hiStart and loStart
                gt1File._hiStart = segment._loAddress;
                gt1File._loStart = segment._segmentSize;
                break;
            }

            // Expecting a valid segment
            if(segment._segmentSize == 0)
            {
                fprintf(stderr, "Loader::loadGt1File() : bad header in segment %d of '%s'\n", segmentCount, filename.c_str());
                return false;
            }

            // Read segment
            segment._dataBytes.resize(segment._segmentSize);
            infile.read((char *)&segment._dataBytes[0], segment._segmentSize);
            if(infile.eof() || infile.bad() || infile.fail())
            {
                fprintf(stderr, "Loader::loadGt1File() : bad segment %d in '%s'\n", segmentCount, filename.c_str());
                return false;
            }

            gt1File._segments.push_back(segment);
            segmentCount++;
        }

        return true;
    }

    bool saveGt1File(const std::string& filepath, Gt1File& gt1File)
    {
        if(gt1File._segments.size() == 0)
        {
            fprintf(stderr, "Loader::saveGt1File() : zero segments, not saving!\n");
            return false;
        }

        std::string filename;
        size_t i = filepath.rfind('.');
        filename = (i != std::string::npos) ? filepath.substr(0, i) + ".gt1" : filepath + ".gt1";

        std::ofstream outfile(filename, std::ios::binary | std::ios::out);
        if(!outfile.is_open())
        {
            fprintf(stderr, "Loader::saveGt1File() : failed to open '%s'\n", filename.c_str());
            return false;
        }

        // Sort segments from lowest address to highest address
        std::sort(gt1File._segments.begin(), gt1File._segments.end(), [](const Gt1Segment& segmentA, const Gt1Segment& segmentB)
        {
            uint16_t addressA = segmentA._loAddress + (segmentA._hiAddress <<8);
            uint16_t addressB = segmentB._loAddress + (segmentB._hiAddress <<8);
            return (addressA < addressB);
        });

        for(int i=0; i<gt1File._segments.size(); i++)
        {
            // Write header
            outfile.write((char *)&gt1File._segments[i]._hiAddress, SEGMENT_HEADER_SIZE);
            if(outfile.bad() || outfile.fail())
            {
                fprintf(stderr, "Loader::saveGt1File() : write error in header of segment %d\n", i);
                return false;
            }

            // Write segment
            outfile.write((char *)&gt1File._segments[i]._dataBytes[0], gt1File._segments[i]._segmentSize);
            if(outfile.bad() || outfile.fail())
            {
                fprintf(stderr, "Loader::saveGt1File() : bad segment %d in '%s'\n", i, filename.c_str());
                return false;
            }
        }

        // Write trailer
        outfile.write((char *)&gt1File._terminator, GT1FILE_TRAILER_SIZE);
        if(outfile.bad() || outfile.fail())
        {
            fprintf(stderr, "Loader::saveGt1File() : write error in trailer of '%s'\n", filename.c_str());
            return false;
        }

        return true;
    }

    void sendByte(uint8_t value, uint8_t& checksum)
    {
        Cpu::setIN(value);
        checksum += value;
    }

    bool sendFrame(int vgaY, uint8_t firstByte, uint8_t* message, uint8_t len, uint16_t address, uint8_t& checksum)
    {
        static LoaderState loaderState = LoaderState::FirstByte;
        static uint8_t payload[PAYLOAD_SIZE];

        bool sending = true;

        switch(loaderState)
        {
            case LoaderState::FirstByte: // 8 bits
            {
                if(vgaY == VSYNC_START+8)
                {
                    for(int i=0; i<len; ++i) payload[i % PAYLOAD_SIZE] = message[i % PAYLOAD_SIZE];
                    sendByte(firstByte, checksum);
                    checksum += firstByte << 6;
                    loaderState = LoaderState::MsgLength;
                }
            }
            break;

            case LoaderState::MsgLength: // 6 bits
            {
                if(vgaY == VSYNC_START+14)
                {
                    sendByte(len, checksum);
                    loaderState = LoaderState::LowAddress;
                }
            }
            break;

            case LoaderState::LowAddress: // 8 bits
            {
                if(vgaY == VSYNC_START+22)
                {
                    sendByte(address & 0x00FF, checksum);
                    loaderState = LoaderState::HighAddress;
                }
            }
            break;

            case LoaderState::HighAddress: // 8 bits
            {
                if(vgaY == VSYNC_START+30)
                {
                    sendByte(address >> 8, checksum);
                    loaderState = LoaderState::Message;
                }
            }
            break;

            case LoaderState::Message: // 8*PAYLOAD_SIZE bits
            {
                static int msgIdx = 0;
                if(vgaY == VSYNC_START+38+msgIdx*8)
                {
                    sendByte(payload[msgIdx], checksum);
                    if(++msgIdx == PAYLOAD_SIZE)
                    {
                        msgIdx = 0;
                        loaderState = LoaderState::LastByte;
                    }
                }
            }
            break;

            case LoaderState::LastByte: // 8 bits
            {
                if(vgaY == VSYNC_START+38+PAYLOAD_SIZE*8)
                {
                    uint8_t lastByte = -checksum;
                    sendByte(lastByte, checksum);
                    checksum = lastByte;
                    loaderState = LoaderState::ResetIN;
                }
            }
            break;

            case LoaderState::ResetIN:
            {
                if(vgaY == VSYNC_START+39+PAYLOAD_SIZE*8)
                {
                    Cpu::setIN(0xFF);
                    loaderState = LoaderState::FirstByte;
                    sending = false;
                }
            }
            break;
        }

        return sending;
    }

    void uploadDirect(void)
    {
        bool hasRomCode = false;
        bool hasRamCode = false;

        uint16_t executeAddress = Editor::getLoadBaseAddress();
        std::string filename = *Editor::getFileName(Editor::getCursorY());
        std::string filepath = std::string("./vCPU/" + filename);

        // Reset video table and reset single step watch address to video line counter
        Graphics::resetVTable();
        Editor::setSingleStepWatchAddress(VIDEO_Y_ADDRESS);

        // Upload raw vCPU code
        if(filename.find(".vcpu") != filename.npos  ||  filename.find(".gt1") != filename.npos)
        {
            Gt1File gt1File;
            if(!loadGt1File(filepath, gt1File)) return;
            executeAddress = gt1File._loStart + (gt1File._hiStart <<8);
            Editor::setLoadBaseAddress(executeAddress);

            for(int j=0; j<gt1File._segments.size(); j++)
            {
                uint16_t address = gt1File._segments[j]._loAddress + (gt1File._segments[j]._hiAddress <<8);
                for(int i=0; i<gt1File._segments[j]._segmentSize; i++)
                {
                    Cpu::setRAM(address+i, gt1File._segments[j]._dataBytes[i]);
                }
            }

            hasRamCode = true;
            hasRomCode = false;
            _disableUploads = false;
        }
        // Upload vCPU assembly code
        else if(filename.find(".vasm") != filename.npos  ||  filename.find(".s") != filename.npos  ||  filename.find(".asm") != filename.npos)
        {
            if(!Assembler::assemble(filepath, DEFAULT_START_ADDRESS)) return;
            executeAddress = Assembler::getStartAddress();
            Editor::setLoadBaseAddress(executeAddress);
            uint16_t address = executeAddress;
            uint16_t customAddress = executeAddress;

            // Save to gt1 format
            Gt1File gt1File;
            gt1File._loStart = address & 0x00FF;
            gt1File._hiStart = (address & 0xFF00) >>8;
            Gt1Segment gt1Segment;
            gt1Segment._loAddress = address & 0x00FF;
            gt1Segment._hiAddress = (address & 0xFF00) >>8;

            Assembler::ByteCode byteCode;
            while(!Assembler::getNextAssembledByte(byteCode))
            {
                (byteCode._isRomAddress) ? hasRomCode = true : hasRamCode = true;

                // Custom address
                if(byteCode._isCustomAddress)
                {
                    if(gt1Segment._dataBytes.size())
                    {
                        // Previous segment
                        gt1Segment._segmentSize = uint8_t(gt1Segment._dataBytes.size());
                        gt1File._segments.push_back(gt1Segment);
                        gt1Segment._dataBytes.clear();
                    }

                    address = byteCode._address;
                    customAddress = address;
                    gt1Segment._loAddress = address & 0x00FF;
                    gt1Segment._hiAddress = (address & 0xFF00) >>8;
                }

                if(!_disableUploads)
                {
                    (byteCode._isRomAddress) ? Cpu::setROM(customAddress, address++, byteCode._data) : Cpu::setRAM(address++, byteCode._data);
                }
                gt1Segment._dataBytes.push_back(byteCode._data);
            }

            // Last segment
            if(gt1Segment._dataBytes.size())
            {
                gt1Segment._segmentSize = uint8_t(gt1Segment._dataBytes.size());
                gt1File._segments.push_back(gt1Segment);
            }

            // Don't save gt1 file for any asm files that contain native rom code
            if(!hasRomCode  &&  !saveGt1File(filepath, gt1File)) return;
        }
        // Invalid file
        else
        {
            fprintf(stderr, "Loader::upload() : invalid file '%s'\n", filename.c_str());
            return;
        }

        // Execute code
        if(!_disableUploads  &&  hasRamCode)
        {
            Cpu::setRAM(0x0016, executeAddress-2 & 0x00FF);
            Cpu::setRAM(0x0017, (executeAddress & 0xFF00) >>8);
            Cpu::setRAM(0x001a, executeAddress-2 & 0x00FF);
            Cpu::setRAM(0x001b, (executeAddress & 0xFF00) >>8);
            //Editor::setSingleStep(true);
        }
        return;
    }

    // TODO: fix the Gigatron version of upload so that it can send more than 60 total bytes, (i.e. break up the payload into multiple packets of 60, 1 packet per frame)
    void upload(int vgaY)
    {
        static bool frameUploading = false;
        static FILE* fileToUpload = NULL;
        static FILE* fileToSave = NULL;
        static uint8_t payload[RAM_SIZE];
        static uint8_t payloadSize = 0;

        if(_startUploading  ||  frameUploading)
        {
            uint16_t executeAddress = Editor::getLoadBaseAddress();
            if(_startUploading)
            {
                _startUploading = false;
                //frameUploading = true;

                uploadDirect();
                return;
            }
            
            static uint8_t checksum = 0;
            static FrameState frameState = FrameState::Resync;

            switch(frameState)
            {
                case FrameState::Resync:
                {
                    if(!sendFrame(vgaY, -1, payload, payloadSize, executeAddress, checksum))
                    {
                        checksum = 'g'; // loader resets checksum
                        frameState = FrameState::Frame;
                    }
                }
                break;

                case FrameState::Frame:
                {
                    if(!sendFrame(vgaY,'L', payload, payloadSize, executeAddress, checksum))
                    {
                        frameState = FrameState::Execute;
                    }
                }
                break;

                case FrameState::Execute:
                {
                    if(!sendFrame(vgaY, 'L', payload, 0, executeAddress, checksum))
                    {
                        checksum = 0;
                        frameState = FrameState::Resync;
                        frameUploading = false;
                    }
                }
                break;
            }
        }
    }

    void disableUploads(bool disable)
    {
        _disableUploads = disable;
    }
}
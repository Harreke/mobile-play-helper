# SPDX-License-Identifier: Apache-2.0
"""Offline recording regression using generated H264/Opus packets; no device required."""
from pathlib import Path
import csv,json,os,struct,subprocess
root=Path(__file__).resolve().parents[1];os.chdir(root)
os.environ['PATH']=str(root/'dist')+os.pathsep+os.environ['PATH']
subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc2=size=160x90:rate=30','-f','lavfi','-i','sine=frequency=880:sample_rate=48000','-t','2','-c:v','libx264','-preset','ultrafast','-bf','0','-g','30','-c:a','libopus','-y','work/recording-fixture.mkv'],check=True)
data=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_packets','-show_data','-of','json','work/recording-fixture.mkv'],encoding='utf-8'))
def binary(dump):
    return bytes.fromhex(''.join(line.split(': ',1)[1].split('  ',1)[0].replace(' ','') for line in dump.strip().splitlines()))
video,audio=data['streams'];avcc=binary(video['extradata']);opus=binary(audio['extradata'])
# MediaCodec/scrcpy supplies H264 Annex B configuration and packets.
assert avcc[0]==1
config=b'';pos=6
for count in (avcc[5]&31,None):
    if count is None: count=avcc[pos];pos+=1
    for _ in range(count):
        size=int.from_bytes(avcc[pos:pos+2],'big');pos+=2
        config+=b'\0\0\0\1'+avcc[pos:pos+size];pos+=size
with open('work/recording-packets.bin','wb') as f:
    f.write(struct.pack('<6I',video['width'],video['height'],int(audio['sample_rate']),audio['channels'],len(config),len(opus)))
    f.write(config+opus)
    for packet in data['packets']:
        stream=packet['stream_index'];payload=binary(packet['data'])
        if not stream:
            converted=b'';pos=0
            while pos<len(payload):
                size=int.from_bytes(payload[pos:pos+4],'big');pos+=4
                converted+=b'\0\0\0\1'+payload[pos:pos+size];pos+=size
            payload=converted
        timing=[round(float(packet.get(key,'0'))*1000000) for key in ('pts_time','dts_time','duration_time')]
        timing[0]+=2000000;timing[1]+=2000000
        f.write(struct.pack('<3I3q',stream,int('K' in packet['flags']),len(payload),*timing));f.write(payload)
subprocess.run(['work/w64devkit/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Iwork','-Iapp/src','-Iwork/SDL3-3.4.12/x86_64-w64-mingw32/include','-Iwork/ffmpeg-8.1.2','app/tests/test_mph_recording.c','dist/SDL3.dll','dist/avcodec-62.dll','dist/avformat-62.dll','dist/avutil-60.dll','-o','work/test_mph_recording.exe'],check=True)
result=subprocess.check_output(['work/test_mph_recording.exe','work/recording-packets.bin'],encoding='utf-8');print(result)
for line in result.splitlines():
    if not line.startswith('RECORDING='):continue
    video_path=Path(line.split('=',1)[1]);meta=json.loads((video_path.parent/'metadata.json').read_text(encoding='utf-8'))
    assert meta['video_packets']==30 and meta['audio_packets']>40,meta
    expected=min(round(float(p['pts_time'])*1000000)+2000000 for p in data['packets'] if p['stream_index']==0 and 'K' in p['flags'] and float(p['pts_time'])>=0.2)
    assert meta['media_origin_pts_us']==expected,meta
    with (video_path.parent/'events.csv').open(encoding='utf-8',newline='') as f: rows=list(csv.DictReader(f))
    assert any(row['event']=='touch' for row in rows)
    assert any(row['event']=='packet' for row in rows)
    info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(video_path)],encoding='utf-8'))
    assert [s['codec_name'] for s in info['streams']]==['h264','opus']
    subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(video_path),'-map','0','-f','null','-'],check=True)
print('Recorded files verified: both tracks decode; keyframe origin and diagnostic CSV agree.')

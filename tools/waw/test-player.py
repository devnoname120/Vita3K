from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/waw-checks/player'
parser = argparse.ArgumentParser()
parser.add_argument('--mode', choices=('timestamps', 'drain', 'loop', 'all'), default='all')
args = parser.parse_args()
OUT.mkdir(parents=True, exist_ok=True)
fixture = OUT / '24fps-90khz.mp4'
if not fixture.exists():
    subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=24',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '1',
                    '-c:v', 'libx264', '-bf', '2', '-g', '24', '-threads', '1',
                    '-video_track_timescale', '90000', '-c:a', 'aac', '-ac', '2', str(fixture)], check=True)
command = ['clang++', '-std=c++23', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', '-DSPDLOG_FMT_EXTERNAL', '-DSPDLOG_COMPILED_LIB', '-DSPDLOG_NO_THREAD_ID']
for include in ('vita3k/codec/include', 'vita3k/util/include', 'external/ffmpeg/include',
                'external/boost', 'external/fmt/include', 'external/spdlog/include'):
    command += ['-I', str(ROOT / include)]
command += [str(ROOT / 'tools/waw/test-player.cpp')]
command += [str(ROOT / 'vita3k/codec/src' / name) for name in ('player.cpp', 'h264.cpp', 'decoder.cpp')]
for lib in ('avformat','avcodec','swscale','avutil','swresample'):
    command += [str(ROOT / 'build/macos-xcode/external/ffmpeg/lib' / ('lib'+lib+'.a'))]
for lib in ('vita3k/util/RelWithDebInfo/libutil.a', 'external/boost/lib/libboost_filesystem.a',
            'external/boost/lib/libboost_atomic.a', 'external/spdlog/RelWithDebInfo/libspdlog.a',
            'external/fmt/RelWithDebInfo/libfmt.a', 'external/RelWithDebInfo/libtracy.a'):
    command += [str(ROOT / 'build/macos-xcode' / lib)]
for framework in ('CoreServices','CoreFoundation','AudioUnit','AudioToolbox','CoreAudio','CoreMedia',
                  'VideoToolbox','CoreVideo','Security'):
    command += ['-framework', framework]
command += ['-lz', '-o', str(OUT / 'test-player')]
subprocess.run(command, check=True)
for mode in (('timestamps', 'drain', 'loop') if args.mode=='all' else (args.mode,)):
    subprocess.run([str(OUT / 'test-player'), mode, str(fixture)], check=True)

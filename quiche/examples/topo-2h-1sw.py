#!/usr/bin/python

from mininet.topo import Topo
from mininet.net import Mininet
from mininet.node import CPULimitedHost
from mininet.link import TCLink
from mininet.util import dumpNodeConnections
from mininet.log import setLogLevel
from mininet.cli import CLI
import time
import subprocess
import re
import numpy as np
import matplotlib.pyplot as plt
import os

class SingleSwitchTopo(Topo):
    "Single switch connected to n hosts."
    def __init__(self, bandwidth, n=2,**opts):
        Topo.__init__(self, **opts)
        switch = self.addSwitch('s1')
        for h in range(n):
            #Each host gets 50%/n of system CPU
            host = self.addHost('h%s' % (h + 1), cpu=.5/n)
            #10 Mbps, 5ms delay, 0% Loss, 1000 packet queue
            self.addLink(host, switch, bw=bandwidth, delay='5ms', loss=0, max_queue_size=1000, use_htb=True)

def perfTest(bandwidth):
    "Create network and run simple performance test"
    topo = SingleSwitchTopo(bandwidth, n=2)
    net = Mininet(topo=topo,host=CPULimitedHost, link=TCLink)
    net.start()
    #print("Dumping host connections")
    dumpNodeConnections(net.hosts)
    #print("Testing network connectivity")
    #net.pingAll()
    #print("Testing bandwidth between h1 and h4")
    h1, h2 = net.get('h1', 'h2')
    #net.iperf((h1, h2))
    #h1.cmd('python3 /home/cyf/gstreamer/gst-env.py &')
    #h2.cmd('python3 /home/cyf/gstreamer/gst-env.py &')
    #h1.cmd('python3 /opt/gstreamer/gst-env.py &')
    #h2.cmd('python3 /opt/gstreamer/gst-env.py &')
    #h1.cmd('export GST_DEBUG=3')
    #h1.cmd('export GST_DEBUG_FILE=send.log')
    #h1.cmd('export GST_DEBUG_NO_COLOR=1')

    #h2.cmd('export GST_DEBUG=3')
    #h2.cmd('export GST_DEBUG_FILE=recv.log')
    #h2.cmd('export GST_DEBUG_NO_COLOR=1')
    time.sleep(2)

    h1.cmdPrint('./gserver2 10.0.0.1 23333 0 2 2>srv.log &')
    #time.sleep(5)
    h2.cmdPrint('./gclient2 10.0.0.1 23333 0 2 2>cli.log &')
    #CLI(net)
    time.sleep(75)
    net.stop()

def cal_ssim(original, distorted, ssim_file):
    #cmd = '/home/cyf/bin/ffmpeg -i ' + distorted + ' -i ' + original + ' -lavfi ssim=stats_file=ssim_logfile.txt -f null -'
    cmd = '/opt/ffmpeg-5.1.1-amd64-static/ffmpeg -i ' + distorted + ' -i ' + original + ' -lavfi ssim=stats_file=' + ssim_file + ' -f null -'
    _, output = subprocess.getstatusoutput(cmd)
    result = output.split('\n')[-1]
    #print("\n------------" + original + " " +distorted + "------------")
    print(result)
    number = re.findall(r"(?<=All\:)\s*\d+\.?\d+", result)

    if not number:
        return -1
    else:
        number = float(number[0])
        return number

def gen_ppl(filesrc):
    lines = []
    with open("ppl.txt","r") as f:
        lines = f.readlines()
    lines[0] = 'filesrc location=./video_src/video_seg_60sec/' + filesrc + ' ! decodebin ! videoconvert ! openh264enc name=openh264enc bitrate=3000000 max-bitrate=10000000 gop-size=100 scene-change-detection=false ! tee name=t t. ! queue ! rtph264pay name=rtph264pay mtu=500000 aggregate-mode=none ! appsink name=appsink sync=true t. ! queue ! h264parse ! avdec_h264 ! videorate ! video/x-raw,framerate=25/1 ! matroskamux ! filesink location=./video_src/send_0.yuv sync=true\n'
    lines[5] = 'filesrc location=./video_src/video_seg_60sec/' + filesrc + ' ! decodebin ! videoconvert ! openh264enc name=openh264enc bitrate=3000000 max-bitrate=10000000 gop-size=100 scene-change-detection=false ! tee name=t t. ! queue ! rtph264pay name=rtph264pay mtu=500000 aggregate-mode=none ! appsink name=appsink sync=true t. ! queue ! h264parse ! avdec_h264 ! videorate ! video/x-raw,framerate=25/1 ! matroskamux ! filesink location=./video_src/send_1.yuv sync=true\n'


    with open("ppl.txt", 'w') as f:
        f.writelines(lines)

def create_directory(directory_path):
    if os.path.exists(directory_path):
        return None
    else:
        try:
            os.makedirs(directory_path)
        except:
            # in case another machine created the path meanwhile !:(
            return None
        return directory_path

def save_log(dir_name, log_path='./log/'):
    create_directory(log_path + dir_name)
    subprocess.getstatusoutput('cp ./srv.log ./cli.log ./ssim_1.txt ./ssim_2.txt ' + log_path + dir_name)

def save_ssim(ssim1, ssim2, path):
    ssim1_s = np.array(ssim1)
    np.save(path + 'ssim_1.npy', ssim1_s)

    ssim2_s = np.array(ssim2)
    np.save(path + 'ssim_2.npy', ssim2_s)

if __name__ == '__main__':
    #video_list = ['720p_2_gop_0.mp4', '720p_2_gop_1.mp4', '1080p_gop_0.mp4', '1080p_gop_1.mp4', '720p_gop_0.mp4', '720p_gop_1.mp4']
    video_list = ['720p_2_gop_1.mp4']
    video_id = -1
    N_TRAIN = 5
    # bw_list = [12, 9.5, 9, 8.5, 8, 7.5, 7]
    bw_list = [10, 9]

    for video in video_list:
        video_id += 1
        gen_ppl(video)
        ssim_0_list = []
        ssim_1_list = []
        for bw in bw_list:
            test_0_list = []
            test_1_list = []
            for i in range(N_TRAIN):
                # setLogLevel('info')
                # delete
                subprocess.getstatusoutput(
                    'rm -f ./video_src/send_0.yuv ./video_src/send_1.yuv ./video_src/recv_0.yuv ./video_src/recv_1.yuv')
                print('*******************************')
                print('********Video {} No.{}/{}, BW {}********'.format(video, i + 1, N_TRAIN, bw))
                print('*******************************')
                perfTest(bw)
                time.sleep(5)
                tmp_0 = cal_ssim("./video_src/send_0.yuv", "./video_src/recv_0.yuv", "ssim_1.txt")
                tmp_1 = cal_ssim("./video_src/send_1.yuv", "./video_src/recv_1.yuv", "ssim_2.txt")

                #if tmp_0 == -1 or tmp_1 == -1:
                #    continue
                test_0_list.append(tmp_0)
                test_1_list.append(tmp_1)

                # save log
                dir_name = "Video" + video + "/BW" + str(bw) + "/iter" + str(i)
                save_log(dir_name)
            save_ssim(test_0_list, test_1_list, "./log/Video" + video + "/BW" + str(bw))
            #print(test_0_list)
            #print(test_1_list)

            ssim_0_list.append(np.mean(test_0_list))
            ssim_1_list.append(np.mean(test_1_list))

        print(ssim_0_list)
        print(ssim_1_list)
        save_ssim(ssim_0_list, ssim_1_list, "./log/Video" + video + "/")




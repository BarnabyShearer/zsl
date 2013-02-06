#!/usr/bin/env python
# -*- coding: utf-8 -*-
# 
# Curses interface to control UVC cameras
# 
# Copyright 2013 Barnaby Shearer <b@zi.is>
# License GPLv2

import curses
import curses.ascii
import ConfigParser
import os
import fcntl
import ctypes
import time

class v4l2_control(ctypes.Structure):
    _fields_ = [
        ('id', ctypes.c_uint32),
        ('value', ctypes.c_int32),
    ]

VIDIOC_S_CTRL = (
    ctypes.c_int32(28 << 0).value |
    ctypes.c_int32(ord('V') << 8).value |
    ctypes.c_int32(ctypes.sizeof(v4l2_control) << 16).value |
    ctypes.c_int32(3 << 30).value #RW
)

screen = None
config = None

class Menu():
    selected = 0
    title = 'x'
    help = None
    action = None
    steps = None
    items = None

    def __init__(self, config, parent = None):
        
        self.title = config['TITLE']
        if 'HELP' in config:
            self.help = config['HELP']
        if 'SELECTED' in config:
            self.selected = config['SELECTED']
        if 'ACTION' in config:
            self.action = config['ACTION']
        if 'STEPS' in config:
            self.steps = config['STEPS']
        if 'ITEMS' in config:
            self.items = []
            self.action = self.draw
            if parent==None:
                config['ITEMS'].append({'TITLE':' Exit', 'ACTION': exit})
            else:
                config['ITEMS'].append({'TITLE':' Back', 'ACTION': parent.draw})
            for item in config['ITEMS']:
                self.items.append(Menu(item, self))

    def navigate(self, i):
        self.select(self.selected + i)

    def select(self, i):
        self.selected = i;
        self.selected %= len(self.items)
        if self.selected < 0:
            self.selected += len(self.items)
        self.redraw()
    
    def draw(self, _ = None):
        self.redraw()
        self.input()

    def redraw(self):
        y = 5
        screen.clear()
        screen.border(0)
        screen.addstr(2,2, self.title, curses.A_STANDOUT)
        if self.help != None:
            y+=1
            screen.addstr(4,2, self.help, curses.A_BOLD)
        i = 0
        for item in self.items:
            if item.title[0] == ' ':
                y+=1
            buf = str(i) + ' - ' + item.title
            if self.items[i].steps != None:
                buf += '\t'
                buf += str(self.items[i].selected) if self.items[i].selected >= 0 else '(Auto)'
                buf += '\t['
                buf += ''.join(['#' if self.items[i].selected == step else '-' for step in self.items[i].steps])
                buf += ']'
            screen.addstr(i+y, 4, buf, (self.selected==i and curses.color_pair(1) or curses.A_NORMAL));
            i += 1
        screen.refresh()

    def input(self):
        while True:
            key = screen.getch()
            item = self.items[self.selected]
            if key == curses.ascii.NL:
                item.action(item.selected)
            elif key == curses.ascii.ESC:
                self.items[-1].action()
            elif key == curses.KEY_DOWN:
                self.navigate(1)
            elif key == curses.KEY_UP:
                self.navigate(-1)
            elif key == curses.KEY_LEFT:
                if item.steps != None:
                    item.selected = item.steps[item.steps.index(item.selected)-1]
                    item.action(item.selected)
                self.redraw()
            elif key == curses.KEY_RIGHT:
                if item.steps != None:
                    item.selected = item.steps[(item.steps.index(item.selected)+1) % len(item.steps)]
                    item.action(item.selected)
                self.redraw()
            elif key >= ord('0') and key <= ord(str(len(self.items))):
                if self.items[key-ord('0')].steps == None:
                    self.items[key-ord('0')].action()
                else:
                    self.select(key-ord('0'))

def set(control, value):
    #HACK: Enable/Disable autos
    if control == '0x009a0902':
        if value == -1:
            set('0x009a0901', 3)
            return
        else:
            set('0x009a0901', 1)
    if control == '0x009a090a':
        if value == -1:
            set('0x009a090c', 1)
            return
        else:
            set('0x009a090c', 0)

    with open('/dev/video0') as fd:
        ctrl = v4l2_control()
        ctrl.id = int(control, 16)
        ctrl.value = int(value)
        fcntl.ioctl(fd, VIDIOC_S_CTRL, ctrl) 

    config.set('Camera', control, str(value))

    #HACK: changing exposure breaks brightness
    if control == '0x009a0902':
        bright = config.get('Camera', '0x00980900')
        set('0x00980900', 30)
        time.sleep(.1)
        set('0x00980900', bright)

def main():
    global screen, config

    config = ConfigParser.ConfigParser();
    config.add_section('Camera')
    for k, v in {
        '0x00980900': '205', #Brightness
        '0x00980901': '0',   #Contrast
        '0x00980902': '60',  #Saturation
        '0x0098091b': '25',  #Sharpness
        '0x009a0901': '1',   #Disable Auto Exposure
        '0x009a0902': '20',  #Exposure
        '0x009a090a': '0',   #Focus
        '0x009a090c': '0',   #Disable Auto Focus
    }.items():
        config.set('Camera', k, v)
    config.read([os.path.expanduser('~/.zsl/camera.cfg')])
    for k,v in config.items('Camera'):
        set(k, v)

    screen = curses.initscr()
    try:
        curses.noecho()
        curses.cbreak()
        curses.start_color()
        screen.keypad(1)
        curses.init_pair(1,curses.COLOR_BLACK, curses.COLOR_WHITE)

        Menu({
            'TITLE': 'ZSL Stereo v0.1',
            'ITEMS': [
                {
                    'TITLE': 'Camera Settings',
                    'HELP': 'Set the exposure as low as posible to avoid motion-blur. Auto settings not recomended.',
                    'ITEMS': [
                        {'TITLE':'Brightness', 'SELECTED':config.getint('Camera','0x00980900'), 'STEPS':range(30,256,25), 'ACTION':lambda x: set('0x00980900',x)},
                        {'TITLE':'Contrast', 'SELECTED':config.getint('Camera','0x00980901'), 'STEPS':range(0,11), 'ACTION':lambda x: set('0x00980901',x)},
                        {'TITLE':'Saturation', 'SELECTED':config.getint('Camera','0x00980902'), 'STEPS':range(0,201,20), 'ACTION':lambda x: set('0x00980902',x)},
                        {'TITLE':'Sharpness', 'SELECTED':config.getint('Camera','0x0098091b'), 'STEPS':range(0,51,5), 'ACTION':lambda x: set('0x0098091b',x)},
                        {
                            'TITLE':'Exposure',
                            'SELECTED':config.getint('Camera','0x009a0902'),
                            'STEPS':[-1, 5,10,20,39,78,156,312,625,1250,2500,5000,10000,20000],
                            'ACTION':lambda x: set('0x009a0902',x)
                        },
                        {'TITLE':'Focus\t', 'SELECTED':config.getint('Camera','0x009a090a'), 'STEPS':[-1] + range(0,41,4), 'ACTION':lambda x: set('0x009a090a',x)}
                    ]
                }
            ]
        }).draw()

    finally:
        curses.endwin()
        if not(os.path.exists(os.path.expanduser('~/.zsl/'))):
            os.mkdir(os.path.expanduser('~/.zsl/'))
        with open(os.path.expanduser('~/.zsl/camera.cfg'), 'wb') as configfile:
            config.write(configfile)

if __name__=="__main__":
    main()

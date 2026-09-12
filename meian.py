#!/usr/bin/env python
# -*- coding: utf8 -*-
#
# Meain TCP protocol client
#
# Copyright (C) 2018, Andrea Tuccia
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#

from __future__ import division, print_function, absolute_import
from xml.parsers.expat import ExpatError
import asyncio
import socket
import binascii
from collections import OrderedDict as OD
import dicttoxml
import re
import socket
import time
import threading
import uuid
import xml.etree.ElementTree as ET
import xmltodict

class ConnectionError(Exception):
    pass

class PushClientError(Exception):
    pass

class LoginError(Exception):
    pass

class ResponseError(Exception):
    pass

class MeianClient():

    seq = 0
    timeout = 10

    def __init__(self, host, port, uid, pwd):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        try:
            self.sock.connect((host, port))
        except socket.timeout:
            self.sock.close()
            raise ConnectionError("Connection error")
        cmd = OD()
        cmd['Id'] = STR(uid)
        cmd['Pwd'] = PWD(pwd)
        cmd['Type'] = 'TYP,ANDROID|0'
        cmd['Token'] = STR(str(uuid.uuid4()))
        cmd['Action'] = 'TYP,IN|0'
        cmd['Err'] = None
        xpath = '/Root/Pair/Client'
        root = self._create(xpath, cmd)
        self.client = self._(xpath, cmd)
        if self.client['Err']:
            raise ClientError("Login error")

    def __del__(self):
        self.sock.close()

    def GetAlarmStatus(self):
        cmd = OD()
        cmd['DevStatus'] = None
        cmd['Err'] = None
        xpath = '/Root/Host/GetAlarmStatus'
        return self._(xpath, cmd)


    def SetAlarmStatus(self, status):
        cmd = OD()
        cmd['DevStatus'] = TYP(status, ['ARM', 'DISARM', 'STAY', 'CLEAR'])
        cmd['Err'] = None
        xpath = '/Root/Host/SetAlarmStatus'
        return self._(xpath, cmd)

    def _(self, xpath, cmd, is_list = False, offset = 0, l = None):
        if offset > 0:
            cmd['Offset'] = S32(offset)
        root = self._create(xpath, cmd)
        self._send(root)
        resp = self._receive()
        if is_list == False:
            return self._select(resp, xpath)
        if l is None:
            l = []
        total = self._select(resp, '%s/Total' % xpath)
        ln = self._select(resp, '%s/Ln' % xpath)
        for i in list(range(ln)):
            event = self._select(resp, '%s/L%d' % (xpath, i))
            l.append(self._select(resp, '%s/L%d' % (xpath, i)))
        offset += ln
        if total > offset:
            self._(xpath, cmd, is_list, offset, l)
        return l

    def _send(self, root):
        xml = dicttoxml.dicttoxml(root, attr_type=False, root=False)
        self.seq += 1
        mesg = b'@ieM%04d%04d0000%s%04d' % (len(xml), self.seq, self._xor(xml), self.seq)
        self.sock.send(mesg)

    def _receive(self):
        # Lire l'en-tête pour obtenir la longueur
        header = self.sock.recv(16)  # "@ieM" + 4 (length) + 4 (seq) + 4 (séparateur)
        if len(header) < 16:
            raise ResponseError("En-tête incomplet")
    
        if header[:4] != b'@ieM':
            raise ResponseError(f"En-tête invalide : {header}")
    
        # Extraire la longueur des données XML
        length = int(header[4:8].decode())
        seq = header[8:12].decode()
    
        # Lire le reste du message : xor_data + seq final
        remaining_length = length + 4  # xor_data + seq final (4 octets)
        data = b''
        while len(data) < remaining_length:
            chunk = self.sock.recv(remaining_length - len(data))
            if not chunk:
                raise ConnectionError("Connexion fermée avant la fin du message")
            data += chunk
    
        # Vérifier que le seq final correspond
        if data[-4:] != seq.encode():
            raise ResponseError(f"Numéro de séquence invalide : attendu {seq}, reçu {data[-4:]}")
    
        # Extraire xor_data (sans le seq final)
        xor_data = data[:-4]
    
        # Décoder et parser
        try:
            decoded_data = self._xor(xor_data).decode()
            print(f"XML décodé : {decoded_data}")
            if decoded_data.count('<') > 1 and not decoded_data.startswith('<Root>'):
            # Si le XML a plusieurs racines, l'encapsuler dans <Message>
                decoded_data = f"<Message>{decoded_data}</Message>"
        except UnicodeDecodeError as e:
            raise ResponseError(f"Erreur de décodage : {e}")

        try:
            resp = xmltodict.parse(
                decoded_data,
                xml_attribs=False,
                dict_constructor=dict,
                postprocessor=self._xmlread
            )
        except xml.parsers.expat.ExpatError as e:
            raise ResponseError(f"Erreur de parsing XML : {e}. Données : {decoded_data}")
        # Si on a encapsulé dans Message, on remet Root au niveau
        # attendu par tout le reste du programme.
        if 'Message' in resp:
            message = resp['Message']

            if 'Root' in message:
                return {
                    'Root': message['Root'],
                    'Err': message.get('Err')
                }

            return message

        return resp




#    def _receive(self):
#        try:
#            data = self.sock.recv(1024) 
#            print (data)
#        except socket.timeout:
#            self.sock.close()
#            raise ConnectionError("Connection error")
#        return xmltodict.parse(self._xor(data[16:-4]).decode(), xml_attribs=False, dict_constructor=dict, postprocessor=self._xmlread)

    def _xor(self, input):
        sz = bytearray.fromhex('0c384e4e62382d620e384e4e44382d300f382b382b0c5a6234384e304e4c372b10535a0c20432d171142444e58422c421157322a204036172056446262382b5f0c384e4e62382d620e385858082e232c0f382b382b0c5a62343830304e2e362b10545a0c3e432e1711384e625824371c1157324220402c17204c444e624c2e12')
        buf = bytearray(input)
        for i in range(len(input)):
            ki = i & 0x7f
            buf[i] = buf[i] ^ sz[ki]
        return buf

    def _create(self, path, mydict = {}):
        root = {}
        elem = root
        try:
            plist = path.strip('/').split('/')
            k = len(plist) - 1
            for i, j in enumerate(plist):
                elem[j] = {}
                if i == k:
                    elem[j] = mydict
                elem = elem.get(j)
        except:
            pass
        return root

    def _select(self, mydict, path):
        elem = mydict
        try:
            for i in path.strip('/').split('/'):
                try:
                    i = int(i)
                    elem = elem[i]
                except ValueError:
                    elem = elem.get(i)
        except:
            pass
        return elem

    def _xmlread(self, path, key, value):
        try:
            input = value
            BOL = re.compile(r'BOL\|([FT])')
            DTA = re.compile(r'DTA(,\d+)*\|(\d{4}\.\d{2}.\d{2}.\d{2}.\d{2}.\d{2})')
            ERR = re.compile(r'ERR\|(\d{2})')
            GBA = re.compile(r'GBA,(\d+)\|([0-9A-F]*)')
            HMA = re.compile(r'HMA,(\d+)\|(\d{2}:\d{2})')
            IPA = re.compile(r'IPA,(\d+)\|(([0-2]?\d{0,2}\.){3}([0-2]?\d{0,2}))')
            MAC = re.compile(r'MAC,(\d+)\|(([0-9A-F]{2}[:-]){5}([0-9A-F]{2}))')
            NEA = re.compile(r'NEA,(\d+)\|([0-9A-F]+)')
            NUM = re.compile(r'NUM,(\d+),(\d+)\|(\d*)')
            PWD = re.compile(r'PWD,(\d+)\|(.*)')
            S32 = re.compile(r'S32,(\d+),(\d+)\|(\d*)')
            STR = re.compile(r'STR,(\d+)\|(.*)')
            TYP = re.compile(r'TYP,(\w+)\|(\d+)')
            if BOL.match(input):
                bol = BOL.search(input).groups()[0]
                if bol == "T":
                    value = True
                if bol == "F":
                    value = False
            elif DTA.match(input):
                dta = DTA.search(input).groups()[1]
                value =  time.strptime(dta,'%Y.%m.%d.%H.%M.%S')
            elif ERR.match(input):
                value =  int(ERR.search(input).groups()[0])
            elif GBA.match(input):
                value =  bytearray.fromhex(GBA.search(input).groups()[1]).decode()
            elif HMA.match(input):
                hma = HMA.search(input).groups()[1]
                value =  time.strptime(hma,'%H:%M')
            elif IPA.match(input):
                value =  str(IPA.search(input).groups()[1])
            elif MAC.match(input):
                value =  str(MAC.search(input).groups()[1])
            elif NEA.match(input):
                value =  str(NEA.search(input).groups()[1])
            elif NUM.match(input):
                value =  str(NUM.search(input).groups()[2])
            elif PWD.match(input):
                value =  str(PWD.search(input).groups()[1])
            elif S32.match(input):
                value =  int(S32.search(input).groups()[2])
            elif STR.match(input):
                value =  str(STR.search(input).groups()[1])
            elif TYP.match(input):
                value =  int(TYP.search(input).groups()[1])
            else:
                raise ResponseError('Unknown data type %s' % input)
            return key, value
        except (ValueError, TypeError):
            return key, value

class MeianPushClient(asyncio.Protocol, MeianClient):
    keepalive = 60
    timeout = 10

    def __init__(self, host, port, uid, handler):
        if not callable(handler):
            raise AttributeError('handler is not a function')
        self.host = host
        self.port = port
        self.handler = handler
        cmd = OD()
        cmd['Id'] = STR(uid)
        cmd['Err'] = None
        xpath = '/Root/Pair/Push'
        self.mesg = self._create(xpath, cmd)
        self.transport = None
        self.keepalive_task = None

    async def connect(self):
        loop = asyncio.get_event_loop()
        self.transport, _ = await loop.create_connection(
            lambda: self,
            self.host,
            self.port
        )

    def connection_made(self, transport):
        self.transport = transport
        self.keepalive_task = asyncio.create_task(self._keepalive_loop())

    def connection_lost(self, exc):
        if self.keepalive_task:
            self.keepalive_task.cancel()
        self.transport = None

    def data_received(self, data):
        head = data[0:4]

        if head == b'%maI':
            if self.keepalive_task:
                self.keepalive_task.cancel()
            self.keepalive_task = asyncio.create_task(self._keepalive_loop())
        elif head == b'@ieM':
            xpath = '/Root/Pair/Push'
            resp = xmltodict.parse(self._xor(data[16:-4]).decode().replace("<Err>ERR|00</Err>",""), xml_attribs=False, dict_constructor=dict, postprocessor=self._xmlread)
            self.push = self._select(resp, xpath)
            err = self._select(resp, '%s/Err' % xpath)
            if err:
                self.transport.close()
                raise PushClientError("Push subscription error")
        elif head == b'@alA':
            xpath = '/Root/Host/Alarm'
            resp = xmltodict.parse(self._xor(data[16:-4]).decode().replace("<Err>ERR|00</Err>",""), xml_attribs=False, dict_constructor=dict, postprocessor=self._xmlread)
            self.handler(self._select(resp, xpath))
        elif head == b'!lmX':
            xpath = '/Root/Host/Alarm'
            resp = xmltodict.parse(data[16:-4], xml_attribs=False, dict_constructor=dict, postprocessor=self._xmlread)
            self.handler(self._select(resp, xpath))
        else:
            self.transport.close()
            raise ResponseError("Response error")

    async def _keepalive_loop(self):
        while True:
            await asyncio.sleep(self.keepalive)
            if self.transport and not self.transport.is_closing():
                self.transport.write(b'%maI')

    def send_message(self, mesg):
        if self.transport and not self.transport.is_closing():
            xml = dicttoxml.dicttoxml(mesg, attr_type=False, root=False)
            message = b'@ieM%04d%04d0000%s%04d' % (len(xml), 0, self._xor(xml), 0)
            self.transport.write(message)

    async def start(self):
        await self.connect()
        if self.mesg is not None:
            self.send_message(self.mesg)
            self.mesg = None

    def close(self):
        if self.transport:
            self.transport.close()

def BOL(en):
    if en == True:
        return 'BOL|T'
    else:
        return 'BOL|F'

def DTA(t):
    dta = time.strftime('%Y.%m.%d.%H.%M.%S', t)
    return 'DTA,%d|%s' % (len(dta), dta)

def PWD(text):
    return 'PWD,%d|%s' % (len(text), text)

def S32(val, pos = 0):
    return 'S32,%d,%d|%d' % (pos, pos, val)

def MAC(mac):
    return 'MAC,%d|%d' % (len(mac), mac)

def IPA(ip):
    return 'IPA,%d|%d' % (len(ip), ip)

def STR(text):
    text = str(text)
    return 'STR,%d|%s' % (len(text), text)

def TYP(val, typ = []):
    try:
        return 'TYP,%s|%d' % (typ[val], val)
    except IndexError:
        return 'TYP,NONE,|%d' % val

Cid = { '1100': 'Personal ambulance',
        '1101': 'Emergency',
        '1110': 'Fire',
        '1120': 'Emergency',
        '1131': 'Perimeter',
        '1132': 'Burglary',
        '1133': '24 hour',
        '1134': 'Delay',
        '1137': 'Dismantled',
        '1301': 'System AC fault',
        '1302': 'System battery failure',
        '1306': 'Programming changes',
        '1350': 'Communication failure',
        '1351': 'Telephone line fault',
        '1370': 'Circuit fault',
        '1381': 'Detector lost',
        '1384': 'Low battery detector',
        '1401': 'Disarm report',
        '1406': 'Alarm canceled',
        '1455': 'Automatic arming failed',
        '1570': 'Bypass Report',
        '1601': 'Manual communication test reports',
        '1602': 'Communications test reports',
        '3301': 'System AC recovery',
        '3302': 'System battery recovery',
        '3350': 'Communication resumes',
        '3351': 'Telephone line to restore',
        '3370': 'Loop recovery',
        '3381': 'Detector loss recovery',
        '3384': 'Detector low voltage recovery',
        '3401': 'Arming Report',
        '3441': 'Staying Report',
        '3570': 'Bypass recovery',
    }

TZ = {  0: 'GMT-12:00',
        1: 'GMT-11:00',
        2: 'GMT-10:00',
        3: 'GMT-09:00',
        4: 'GMT-08:00',
        5: 'GMT-07:00',
        6: 'GMT-06:00',
        7: 'GMT-05:00',
        8: 'GMT-04:00',
        9: 'GMT-03:30',
       10: 'GMT-03:00',
       11: 'GMT-02:00',
       12: 'GMT-01:00',
       13: 'GMT',
       14: 'GMT+01:00',
       15: 'GMT+02:00',
       16: 'GMT+03:00',
       17: 'GMT+04:00',
       18: 'GMT+05:00',
       19: 'GMT+05:30',
       20: 'GMT+05:45',
       21: 'GMT+06:00',
       22: 'GMT+06:30',
       23: 'GMT+07:00',
       24: 'GMT+08:00',
       25: 'GMT+09:00',
       26: 'GMT+09:30',
       27: 'GMT+10:00',
       28: 'GMT+11:00',
       29: 'GMT+12:00',
       30: 'GMT+13:00',
}

async def main():
    host = '192.168.1.99'
    uid = 'admin'
    pwd = 'MotdePasse'
    port = 18034

    # Utilisation de MeianClient
    myalarm = MeianClient(host, port, uid, pwd) # <Err>ERR|00</Err><Root><Pair><Client><Err></Err></Client></Pair></Root>
    print(myalarm.GetAlarmStatus()) # <Err>ERR|00</Err><Root><Host><GetAlarmStatus><DevStatus>TYP,DISARM|1</DevStatus><Err></Err></GetAlarmStatus></Host></Root>
    print(myalarm.SetAlarmStatus(3)) # <Err>ERR|00</Err><Root><Host><SetAlarmStatus><DevStatus>TYP,CLEAR|3</DevStatus><Err></Err></SetAlarmStatus></Host></Root>

    # Utilisation de MeianPushClient avec asyncio
    def mytest(alarm):
        print(alarm)

    mypush = MeianPushClient(host, port, uid, mytest)
    await mypush.start()
# exemple de message reçu deserialisé en json
#{'Cid': '3441', 'Content': 'System Stay', 'Time': time.struct_time(tm_year=2026, tm_mon=9, tm_mday=12, tm_hour=14, tm_min=58, tm_sec=7, tm_wday=5, tm_yday=255, tm_isdst=-1), 'Zone': 70, 'ZoneName': '', 'Name': 'FC7640', 'Err': None}
#{'Cid': '1401', 'Content': 'System Disarm', 'Time': time.struct_time(tm_year=2026, tm_mon=9, tm_mday=12, tm_hour=14, tm_min=58, tm_sec=26, tm_wday=5, tm_yday=255, tm_isdst=-1), 'Zone': 70, 'ZoneName': '', 'Name': 'FC7640', 'Err': None}
    # Garder le programme en vie
    try:
        while True:
            await asyncio.sleep(60)
    except KeyboardInterrupt:
        mypush.close()

if __name__ == "__main__":
    asyncio.run(main())

#if __name__ == "__main__":
#    # execute only if run as a script
#    main()

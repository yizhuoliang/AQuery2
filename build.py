from dataclasses import dataclass
from enum import Enum, auto
from tabnanny import check
from aquery_config import *
import sys
import os
import subprocess
import hashlib
import pickle
from engine.utils import nullstream
from typing import Dict, Optional, Set, Union
@dataclass
class checksums:
    libaquery_a : Optional[Union[bytes, bool]] = None
    pch_hpp_gch : Optional[Union[bytes, bool]] = None
    server : Optional[Union[bytes, bool]] = None
    sources : Union[Dict[str, bytes], bool] = None
    def calc(self, libaquery_a = 'libaquery.a', 
                pch_hpp_gch = 'server/pch.hpp.gch', 
                server = 'server.so'
        ):
        for key in self.__dict__.keys():
            try:
                with open(eval(key), 'rb') as file:
                    self.__dict__[key] = hashlib.md5(file.read()).digest()
            except (FileNotFoundError, NameError):
                pass
        sources = [*build_manager.headerfiles, *build_manager.sourcefiles]
        self.sources = dict()
        for key in sources:
            try:
                with open(key, 'rb') as file:
                    self.sources[key] = hashlib.md5(file.read()).digest()
            except FileNotFoundError:
                print('missing component: ' + key)
                self.sources[key] = None
    def __ne__(self, __o: 'checksums') -> 'checksums':
        ret = checksums()
        for key in self.__dict__.keys():
            try:
                ret.__dict__[key] = self.__dict__[key] != __o.__dict__[key]
            except KeyError:
                ret.__dict__[key] = True
        return ret
    
    def __eq__(self, __o: 'checksums') -> 'checksums':
        ret = checksums()
        for key in self.__dict__.keys():
            try:
                ret.__dict__[key] = self.__dict__[key] == __o.__dict__[key]
            except KeyError:
                ret.__dict__[key] = False
        return ret

        
        
class build_manager:
    sourcefiles = ['server/server.cpp', 'server/io.cpp', 
                   'server/monetdb_conn.cpp', 'server/threading.cpp',
                   'server/winhelper.cpp'
                   ]
    headerfiles = ['server/aggregations.h', 'server/hasher.h', 'server/io.h',
                   'server/libaquery.h', 'server/monetdb_conn.h', 'server/pch.hpp',
                   'server/table.h', 'server/threading.h', 'server/types.h', 'server/utils.h',
                   'server/winhelper.h', 'server/gc.hpp', 'server/vector_type.hpp', 
                   'server/table_ext_monetdb.hpp'
                   ]
   
    class DriverBase:
        def __init__(self, mgr : 'build_manager') -> None:
            self.mgr = mgr
            self.build_cmd = ['']
        def libaquery_a(self) :
            pass
        def pch(self):
            pass
        def build(self, stdout = sys.stdout, stderr = sys.stderr):
            for c in self.build_cmd:
                subprocess.call(c, stdout = stdout, stderr = stderr)
                
    class MakefileDriver(DriverBase):
        def __init__(self, mgr : 'build_manager') -> None:
            super().__init__(mgr)
            os.environ['PCH'] = f'{mgr.PCH}'
            os.environ['CXX'] = mgr.cxx if mgr.cxx else 'c++'
        
        def libaquery_a(self):
            self.build_cmd = [['rm', 'libaquery.a'],['make', 'libaquery.a']]
            return self.build()
        def pch(self):
            self.build_cmd = [['rm', 'server/pch.hpp.gch'], ['make', 'pch']]
            return self.build()
        def server(self):
            if self.mgr.StaticLib:
                self.build_cmd = [['rm', 'server.so'], ['make', 'server_uselib']]
            else:
                self.build_cmd = [['rm', 'server.so'], ['make', 'server.so']]
            return self.build()
            
        def snippet(self):
            if self.mgr.StaticLib:
                self.build_cmd = [['make', 'snippet_uselib']]
            else:
                self.build_cmd = [['rm', 'dll.so'], ['make', 'snippet']]
            return self.build()
        
    class PythonDriver(DriverBase):
        def __init__(self, mgr : 'build_manager') -> None:
            super().__init__(mgr)
            
    class Driver(Enum):
        Makefile = auto()
        Python = auto()
        CMake = auto() 
        MSBuild = auto()
 
    def __init__(self) -> None:
        self.method = 'make'
        self.cxx = ''
        self.OptimizationLv = '4' # [O0, O1, O2, O3, Ofast]
        self.Platform = 'x64'
        self.PCH = 1
        self.StaticLib = 1
        self.fLTO = 1
        self.fmarchnative = 1
        self.driver = build_manager.MakefileDriver(self)
        self.cache_status = checksums()
        
    def build_dll(self):
        return self.driver.snippet()
        
    def build_caches(self, force = False):
        cached = checksums()
        current = checksums()
        current.calc()
        try:
            with open('.cached', 'rb') as cache_sig:
                cached = pickle.loads(cache_sig.read())
        except FileNotFoundError:
            pass
        self.cache_status = current != cached
        
        if  force or self.cache_status.sources:
            self.driver.pch()
            self.driver.libaquery_a()
            self.driver.server()
        else:
            if self.cache_status.libaquery_a:
                self.driver.libaquery_a()
            if self.cache_status.pch_hpp_gch:
                self.driver.pch()
            if self.cache_status.server:
                self.driver.server()
        current.calc()
        with open('.cached', 'wb') as cache_sig:
            cache_sig.write(pickle.dumps(current))
            
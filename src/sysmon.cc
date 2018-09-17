/*
    OGN - Open Glider Network - http://glidernet.org/
    Copyright (c) 2015 The OGN Project

    A detailed list of copyright holders can be found in the file "AUTHORS".

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <ogn-rf/sysmon.h>

int getMemoryUsage(int &Total, int &Free) // get total and free RAM [kB]
{ FILE *File=fopen("/proc/meminfo","rt"); if(File==0) return -1;
  char Line[64];
  if(fgets(Line, 64, File)==0) goto Error;
  if(memcmp(Line, "MemTotal:", 9)!=0) goto Error;
  Total = atoi(Line+10);
  if(fgets(Line, 64, File)==0) goto Error;
  if(memcmp(Line, "MemFree:", 8)!=0) goto Error;
  Free = atoi(Line+10);
  fclose(File); return 0;
Error:
  fclose(File); return -1; }

int getCpuUsage(int &DiffTotal, int &DiffUser, int &DiffSystem) // get CPU usage
{ static int RefTotal=0, RefUser=0, RefSystem=0;
  FILE *File=fopen("/proc/stat","rt"); if(File==0) return -1;
  char Line[64];
  if(fgets(Line, 64, File)==0) goto Error;
  if(memcmp(Line, "cpu ", 4)!=0) goto Error;
  int Total, User, Nice, System, Idle;
  if(sscanf(Line+4, "%d %d %d %d", &User, &Nice, &System, &Idle)!=4) goto Error;
  User+=Nice;
  Total = User+System+Idle;
  DiffTotal=Total-RefTotal; DiffUser=User-RefUser; DiffSystem=System-RefSystem;
  RefTotal=Total; RefUser=User; RefSystem=System;
  fclose(File); return 0;
Error:
  fclose(File); return -1; }

int getCpuSerial(long long int &SerialNumber) // get the CPU serial number
{ FILE *File=fopen("/proc/cpuinfo","rt"); if(File==0) return -1;
  char Line[128];
  for( ; ; )
  { if(fgets(Line, 128, File)==0) goto Error;
    if(memcmp(Line, "Serial", 6)==0) break;
  }
  if(sscanf(Line+6, " : %llx", &SerialNumber)!=1) goto Error;
  fclose(File); return 0;
Error:
  fclose(File); return -1; }

int getCpuUsage(void)                       // get CPU usage - initialize
{ int DiffTotal, DiffUser, DiffSystem; return getCpuUsage(DiffTotal, DiffUser, DiffSystem); }

int getCpuUsage(float &User, float &System) // get CPU usage as ratios
{ int DiffTotal, DiffUser, DiffSystem;
  int Error=getCpuUsage(DiffTotal, DiffUser, DiffSystem); if(Error<0) return Error;
  if(DiffTotal<=0) return -1;
  User = (float)DiffUser/DiffTotal; System = (float)DiffSystem/DiffTotal; return 0; }



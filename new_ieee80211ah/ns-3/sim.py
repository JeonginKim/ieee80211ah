import os
import sys

#payload = [49, 54, 59, 64, 69]
#payload = [34, 39, 44, 66, 67]
#payload = [59,54,49,44,39,34]
payload = [60]

for i in payload:
		command = "./waf --run \" scratch/s1g-mac-test --NRawSta=250 --NGroup=1 --SlotFormat=0 --NRawSlotCount=162 --NRawSlotNum=1 --DataMode=\"OfdmRate650KbpsBW2MHz\" --datarate=0.65 --bandWidth=2 --rho=50 --simulationTime=600 --payloadSize="+str(i)+" --BeaconInterval=100000 --UdpInterval=0.8 --Nsta=250 --seed=1 \" > trace"+str(i)+"byte"
		os.system(command);
		
#!/usr/bin/env python3

import os
import sys
import subprocess

specs = ['500.perlbench_r1', '500.perlbench_r2', '500.perlbench_r3', '502.gcc_r1', '502.gcc_r2', '502.gcc_r3', '502.gcc_r4', '502.gcc_r5',
         '503.bwaves_r1', '503.bwaves_r2', '503.bwaves_r3', '503.bwaves_r4', '505.mcf_r1', '507.cactuBSSN_r1', '508.namd_r1', '510.parest_r1', '511.povray_r1',
         '519.lbm_r1', '520.omnetpp_r1', '521.wrf_r1', '523.xalancbmk_r1', '525.x264_r1', '525.x264_r2', '525.x264_r3', '526.blender_r1', '527.cam4_r1',
         '531.deepsjeng_r1', '538.imagick_r1', '541.leela_r1', '544.nab_r1', '548.exchange2_r1', '549.fotonik3d_r1', '554.roms_r1', '557.xz_r1', '557.xz_r2', '557.xz_r3',
	 '600.perlbench_s1', '600.perlbench_s2', '600.perlbench_s3', '605.mcf_s1', '619.lbm_s1', '620.omnetpp_s1', '623.xalancbmk_s1', '625.x264_s1', '625.x264_s2',
	 '602.gcc_s1', '602.gcc_s2', '602.gcc_s3', '625.x264_s3', '631.deepsjeng_s1', '641.leela_s1', '648.exchange2_s1', '649.fotonik3d_s1', '657.xz_s1', '657.xz_s2']

spec_path = '/nvme/benchmarks/binaries/spec/spec-2017_ref/'

def main():
	w = int(sys.argv[4])
	core = sys.argv[5]
	benchmark = sys.argv[6]

	com_dir = os.getcwd()+'/spec2017-commands.txt'
	if (benchmark in specs):
		with open(com_dir, 'r') as com:
			for line in com:
				if (line != '\n' and benchmark in line):
					cmd = (line.split(':'))[1]
					cmd = cmd.strip()
					break
		com.close()
		cmd = 'taskset -c '+core+' '+cmd
		print(cmd)
		os.chdir(spec_path+benchmark[:-1]+'/ref/')
		app = subprocess.Popen('exec ' + cmd, shell=True)
		buf = "{}\0".format(app.pid)
		os.write(w, bytes(buf, 'ascii'))
		os.close(w)
		app.wait()
	else:
		print('Selected benchmark not found')
		buf = "-1\0"
		os.write(w, bytes(buf, 'ascii'))
		os.close(w)
	sys.exit(0)

if __name__ == "__main__":
	main()

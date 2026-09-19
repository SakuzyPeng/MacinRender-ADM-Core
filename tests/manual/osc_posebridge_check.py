"""Real dynamic-library C ABI + actual PoseBridge CLI, no GUI or audio device."""
import argparse
import ctypes as c
import json
import math
from pathlib import Path
import subprocess
import tempfile
import time

class Config(c.Structure):
    _fields_=[('struct_size',c.c_uint32),('listen_port',c.c_uint32),('source_id',c.c_char_p)]
class Pose(c.Structure):
    _fields_=[('struct_size',c.c_uint32),('protocol_version',c.c_uint32),('has_pose',c.c_uint32),('fresh',c.c_uint32)] + [
        (name,c.c_uint64) for name in ['receiver_session_id','receiver_sequence','receiver_received_ns','age_ms',
        'instance_id','source_session_id','source_sequence','tx_sequence','reference_epoch','metadata_revision',
        'source_received_ns','source_age_at_send_ns','sample_time_ms','sample_clock_epoch']] + [
        ('sample_time_kind',c.c_uint32),('quaternion',c.c_float*4),('yaw',c.c_float),('pitch',c.c_float),('roll',c.c_float)]
class Status(c.Structure):
    _fields_=[('struct_size',c.c_uint32),('state',c.c_int32),('bound_port',c.c_uint32),('has_pose',c.c_uint32)] + [
        (name,c.c_uint64) for name in ['session_id','sequence','packets_received','rejected_packets','recovery_count',
        'age_ms','pose_packets','telemetry_packets','ignored_sources','protocol_mismatches','missing_tx_packets']] + [
        ('has_heartbeat',c.c_uint32),('heartbeat_alive',c.c_uint32),('heartbeat_age_ms',c.c_uint64)]

class Receiver:
    def __init__(self, library, source_id=None):
        self.lib=c.CDLL(str(Path(library).resolve())); self.handle=c.c_void_p()
        declarations={
            'adm_create_osc_head_tracking':([c.POINTER(Config),c.POINTER(c.c_void_p)],c.c_int),
            'adm_destroy_osc_head_tracking':([c.c_void_p],None),
            'adm_osc_head_tracking_start':([c.c_void_p],c.c_int),
            'adm_osc_head_tracking_stop':([c.c_void_p],None),
            'adm_osc_head_tracking_get_pose':([c.c_void_p,c.POINTER(Pose)],c.c_int),
            'adm_osc_head_tracking_get_status':([c.c_void_p,c.POINTER(Status)],c.c_int),
            'adm_osc_head_tracking_snapshot_json':([c.c_void_p,c.POINTER(c.c_void_p)],c.c_int),
            'adm_free_string':([c.c_void_p],None),
        }
        for name,(arguments,result) in declarations.items():
            fn=getattr(self.lib,name);fn.argtypes=arguments;fn.restype=result
        assert c.sizeof(Config)==16 and c.sizeof(Pose)==160 and c.sizeof(Status)==120
        assert self.lib.adm_api_version_major()==1 and self.lib.adm_api_version_minor()>=42
        config=Config(c.sizeof(Config),0,source_id.encode() if source_id else None)
        assert self.lib.adm_create_osc_head_tracking(c.byref(config),c.byref(self.handle))==0
        assert self.lib.adm_osc_head_tracking_start(self.handle)==0
    def close(self):
        if self.handle: self.lib.adm_destroy_osc_head_tracking(self.handle);self.handle=c.c_void_p()
    def pose(self):
        value=Pose();value.struct_size=c.sizeof(value)
        assert self.lib.adm_osc_head_tracking_get_pose(self.handle,c.byref(value))==0
        return value
    def status(self):
        value=Status();value.struct_size=c.sizeof(value)
        assert self.lib.adm_osc_head_tracking_get_status(self.handle,c.byref(value))==0
        return value
    def snapshot(self):
        pointer=c.c_void_p();assert self.lib.adm_osc_head_tracking_snapshot_json(self.handle,c.byref(pointer))==0
        try: return json.loads(c.string_at(pointer))
        finally: self.lib.adm_free_string(pointer)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library',type=Path,required=True);parser.add_argument('--posebridge',type=Path,required=True)
    args=parser.parse_args();receiver=Receiver(args.library,'integration');instances=set()
    def simulate(fmt,synthetic=False,pattern='fixed',source='integration'):
        before=receiver.pose().receiver_sequence; samples=[]
        command=[str(args.posebridge.resolve()),'simulate','--source-id',source,'--format',fmt,
            '--yaw','30','--pitch','20','--roll','10','--pattern',pattern,'--duration','1.25',
            '--osc-target',f'127.0.0.1:{receiver.status().bound_port}'] + (['--sample-clock'] if synthetic else [])
        # Files avoid blocking the producer on a small Windows pipe buffer.
        with tempfile.TemporaryFile(mode='w+',encoding='utf-8',dir=args.library.parent) as output:
            process=subprocess.Popen(command,stdout=output,stderr=output,text=True)
            deadline=time.monotonic()+15
            try:
                while process.poll() is None:
                    snapshot=receiver.snapshot();value=snapshot['pose']
                    if value and int(snapshot['receiver_sequence'])>before and (not samples or value['sequence']!=samples[-1]['sequence']):
                        assert snapshot['schema']==3 and snapshot['source_id']=='integration'
                        assert abs(sum(v*v for v in value['quaternion_xyzw'])-1)<1e-5
                        assert value['sample_time_kind']==(2 if synthetic else 0)
                        if synthetic: assert int(value['sample_time_ms'])==int(value['received_ns'])//1000000
                        samples.append(value)
                    assert time.monotonic()<deadline,'producer timeout'
                    time.sleep(.002)
                output.seek(0);assert process.returncode==0,output.read()
            finally:
                if process.poll() is None: process.kill();process.wait()
        if source=='integration':
            assert len(samples)>=25
            instance=samples[-1]['instance_id'];assert instance not in instances;instances.add(instance)
            snap=receiver.snapshot();assert snap['info_matches_pose'] and snap['status_matches_pose']
            assert snap['source_status']['status']['state']=='stopped'
            assert not snap['fresh'] and snap['heartbeat_alive']
        else: assert not samples and receiver.status().ignored_sources>0
        return samples
    try:
        assert not receiver.pose().has_pose
        for fmt in ['quaternion','euler']:
            samples=simulate(fmt,True)
            assert all(abs(a-b)<.001 for a,b in zip(samples[-1]['euler_deg'],[30,20,10]))
        simulate('quaternion',False)
        wrapped=simulate('quaternion',True,'wrap')
        assert any(s['euler_deg'][0]>175 for s in wrapped) and any(s['euler_deg'][0]<-175 for s in wrapped)
        for a,b in zip(wrapped,wrapped[1:]): assert abs(sum(x*y for x,y in zip(a['quaternion_xyzw'],b['quaternion_xyzw'])))>.98
        simulate('quaternion',False,source='other')
        time.sleep(3.1);assert not receiver.snapshot()['heartbeat_alive']
        s=receiver.status();assert s.rejected_packets==0 and s.missing_tx_packets==0
        print(json.dumps({'result':'PASS','protocol':3,'poses':s.pose_packets,'telemetry':s.telemetry_packets,
            'ignored_sources':s.ignored_sources,'source_instances':len(instances),'json_ownership':True,'separate_timeouts':True},indent=2))
    finally: receiver.close()
if __name__=='__main__': main()

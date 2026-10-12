#!/usr/bin/env python3
"""Maintenance-only fixture generation; native loading/tests do not invoke Python."""
from pathlib import Path
import shutil
import h5py
import numpy as np
root = Path(__file__).resolve().parents[1] / 'fixtures/sofa'
for name,rate,seconds in [('room-delayed',44100,False),('room-seconds',44100,True),('room-96k',96000,False)]:
    path=root/(name+'.sofa')
    shutil.copyfile(root/'general-compressed.sofa',path)
    with h5py.File(path,'r+',libver='latest') as f:
        points=f['SourcePosition'][...]
        f['SourcePosition'][...]=np.column_stack((-points[:,1],points[:,0],points[:,2]))+[3,2,1]
        for key,value in [('ListenerPosition',[[3,2,1]]),('ListenerView',[[0,1,0]]),('ListenerUp',[[0,0,1]]),('ReceiverPosition',np.array([[0,-.09,0],[0,.09,0]])[:,:,None])]:
            obj=f.create_dataset(key,data=np.asarray(value,dtype='<f8'));obj.attrs['Type']=np.bytes_('cartesian');obj.attrs['Units']=np.bytes_('metre')
        ir=f['Data.IR'][...][:,::-1].copy();ir[:,:,-1]=.015625;f['Data.IR'][...]=ir
        f['Data.SamplingRate'][...]=rate
        f['Data.Delay'][...]=np.array([[2.5,-.25]])/(rate if seconds else 1)
        del f['Data.Delay'].attrs['Units'];f['Data.Delay'].attrs['Units']=np.bytes_('second' if seconds else 'samples')
        f.flush()

# Spectrograms of WAV files, one row each: python3 tools/spectrogram.py out.png a.wav b.wav
import sys, numpy as np, scipy.io.wavfile as w, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
files=sys.argv[2:]; out=sys.argv[1]
fig,axs=plt.subplots(len(files),1,figsize=(13,2.6*len(files)))
axs=np.atleast_1d(axs)
for ax,f in zip(axs,files):
    sr,x=w.read(f); x=x.astype(float); x/=np.abs(x).max()+1e-9 if x.dtype.kind=='f' else 2**31; m=x.mean(axis=1) if x.ndim>1 else x
    ax.specgram(m,NFFT=2048,Fs=sr,noverlap=1536,cmap='magma',vmin=-150,vmax=-20); ax.set_ylim(0,min(sr/2,20000)); ax.set_title(f.split('/')[-1],fontsize=9)
plt.tight_layout(); plt.savefig(out,dpi=70)

# CoD1 (3ds Max biped default names) -> DoD ValveBiped
def cod1_to_dod():
    m = {'bip01 pelvis':'Bip01_Pelvis','bip01 spine':'Bip01_Spine','bip01 spine1':'Bip01_Spine1','bip01 spine2':'Bip01_Spine4',
         'bip01 neck':'Bip01_Neck1','bip01 head':'Bip01_Head1'}
    for s,S in (('l','L'),('r','R')):
        for a,b in (('thigh','Thigh'),('calf','Calf'),('foot','Foot'),('toe0','Toe0'),('clavicle','Clavicle'),
                    ('upperarm','UpperArm'),('forearm','Forearm'),('hand','Hand')):
            m[f'bip01 {s} {a}']=f'Bip01_{S}_{b}'
        for f in range(5):
            m[f'bip01 {s} finger{f}']=f'Bip01_{S}_Finger{f}'
            m[f'bip01 {s} finger{f}1']=f'Bip01_{S}_Finger{f}1'
            m[f'bip01 {s} finger{f}2']=f'Bip01_{S}_Finger{f}2'
    return {k:'ValveBiped.'+v for k,v in m.items()}

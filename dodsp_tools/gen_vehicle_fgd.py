"""Writes the generated part of content/dodsp_vehicles.fgd from content/scripts/dodsp_vehicles.txt (one owner: the
script). One Hammer class per vehicle type (dodsp_vehicle_<type> - the server registers the same names from the same
script) with its own preview model, plus the plain dodsp_vehicle with a vehicletype list.

usage: gen_vehicle_fgd.py            rewrite the FGD
       gen_vehicle_fgd.py --check    exit 1 if the FGD isn't what the script says (the build's lint runs this)"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'vehicle_preview'))
from editor_models import types_from_script

SCRIPT = os.path.join(HERE, 'content', 'scripts', 'dodsp_vehicles.txt')
FGD = os.path.join(HERE, 'content', 'dodsp_vehicles.fgd')
BEGIN, END = '// BEGIN GENERATED\n', '// END GENERATED\n'

def preview(t, name):
    return ('models/dodsp/editor/%s.mdl' % name) if 'turret_model' in t else t['model']

def generated():
    types = types_from_script(SCRIPT)
    order = list(types)
    out = ['@PointClass base(DoDSPVehicleBase) studio("models/cod2/vehicles/panzer2_hull.mdl") = dodsp_vehicle :\n'
           '\t"DOD-SP armoured vehicle, type chosen by a key (the preview is always the Panzer II). Prefer the dodsp_vehicle_<type> '
           'classes: they show the right vehicle."\n[\n'
           '\tvehicletype(choices) : "Vehicle type" : "%s" : "A block in scripts/dodsp_vehicles.txt." =\n\t[\n' % order[0]]
    out += ['\t\t"%s" : "%s"\n' % (n, types[n].get('display', n)) for n in order]
    out.append('\t]\n]\n\n')
    for n in order:
        t = types[n]
        out.append('@PointClass base(DoDSPVehicleBase) studio("%s") = dodsp_vehicle_%s : "%s - DOD-SP vehicle (scripts/dodsp_vehicles.txt). '
                   'Point its front the way it should start facing." []\n' % (preview(t, n), n, t.get('display', n)))
    return ''.join(out)

def main():
    s = open(FGD, encoding='utf-8').read()
    a, b = s.index(BEGIN) + len(BEGIN), s.index(END)
    new = s[:a] + generated() + s[b:]
    if '--check' in sys.argv:
        if new != s:
            print('dodsp_vehicles.fgd is out of date with dodsp_vehicles.txt - run dodsp_tools/gen_vehicle_fgd.py')
            sys.exit(1)
        return
    open(FGD, 'w', encoding='utf-8', newline='\n').write(new)
    print('wrote %s' % FGD)

if __name__ == '__main__':
    main()

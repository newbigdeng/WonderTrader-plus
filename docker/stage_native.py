"""Collect the supported rebuilt runtime and its matching CTP vendor libraries."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument("build",type=Path)
parser.add_argument("destination",type=Path)
parser.add_argument("--source-root",type=Path,required=True)
args=parser.parse_args()
binary=args.build/"build_x64/Release/bin"
targets={
    "libWtMsgQue.so":"libWtMsgQue.so",
    "libWtPorter.so":"WtPorter/libWtPorter.so",
    "libWtBtPorter.so":"WtBtPorter/libWtBtPorter.so",
    "libWtDtPorter.so":"WtDtPorter/libWtDtPorter.so",
    "libWtDtHelper.so":"WtDtPorter/libWtDtHelper.so",
    "libWtDtServo.so":"WtDtPorter/libWtDtServo.so",
    "libWtDataStorage.so":"libWtDataStorage.so",
    "libWtDataStorageAD.so":"libWtDataStorageAD.so",
    "libWtRiskMonFact.so":"libWtRiskMonFact.so",
    "libCTPLoader.so":"Loader/libCTPLoader.so",
    "LoaderRunner":"Loader/LoaderRunner",
    "parsers/libParserUDP.so":"libParserUDP.so",
    "parsers/libParserShm.so":"libParserShm.so",
    "parsers/libParserCTP.so":"libParserCTP.so",
    "traders/libTraderCTP.so":"libTraderCTP.so",
    "traders/libTraderMocker.so":"libTraderMocker.so",
    "executer/libWtExeFact.so":"libWtExeFact.so",
    "WtUftRunner":"WtUftRunner/WtUftRunner",
    "libWtUftStraFact.so":"WtUftRunner/libWtUftStraFact.so",
}
vendor={
    "parsers/thostmduserapi_se.so":args.source_root/"src/API/CTP6.7.13-mduserapi/thostmduserapi_se.so",
    "traders/thosttraderapi_se.so":args.source_root/"src/API/CTP6.7.13-traderapi/thosttraderapi_se.so",
}
for name,source in {**{name:binary/relative for name,relative in targets.items()},**vendor}.items():
    if not source.is_file():raise FileNotFoundError(source)
    destination=args.destination/name
    destination.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(source,destination)

report=[]
for name in [*targets,*vendor]:
    result=subprocess.run(["ldd","-r",str(args.destination/name)],text=True,capture_output=True)
    output=result.stdout+result.stderr
    if result.returncode or "not found" in output or "undefined symbol" in output:
        raise RuntimeError(name+" has unresolved dependencies:\n"+output)
    if "libnanomsg" in output:raise RuntimeError("Legacy nanomsg dependency: "+name)
    report.append(dict(file=name,origin="CTP 6.7.13 vendor SDK" if name in vendor else "source build"))
(args.destination/"manifest.json").write_text(json.dumps(report,indent=2)+"\n")
print("Staged",len(targets),"rebuilt targets and",len(vendor),"matching vendor libraries")

# EDS conversion input guide

Model: [kinco.template.json](./kinco.template.json)

Search the model file by OD index/subindex for application fields; JSON Pointer paths are included only as a secondary reference for tools. Replace the whole \{USER\_INPUT\} string with a correctly typed JSON value. Examples are illustrative, not confirmed device settings. This guide is not loaded by the simulator.

## Required inputs (4)

| Field / editor search | Expected value | JSON example |
| --- | --- | --- |
| `/node_id` | integer 1..127, or 255 for LSS commissioning | 10 |
| `/bitrate_kbps` | integer 1..1000 | 125 |
| `/worker_period_us` | integer 100..100000 | 1000 |
| `/identity/serial_number` | integer 1..4294967295 | 1001 |

## Input details

### `/node_id`

Choose the simulated slave's CANopen node ID.

### `/bitrate_kbps`

Choose a bitrate supported by both the device and the XNET interface, in kbit/s.

EDS-supported values: 50, 125, 250, 500, 1000.

### `/worker_period_us`

Worker scheduling period in microseconds; 1000 is 1 ms.

### `/identity/serial_number`

Choose a nonzero serial number unique to this simulated device.

## Automatic defaults

- **local\_write:** true for application values; structural subindex 0 stays read-only and not locally writable
- **missing\_od\_values:** 0 for numeric application defaults and supported numeric communication settings; empty string for VISIBLE\_STRING; empty byte array for OCTET\_STRING
- **missing\_transmission\_type:** 0: synchronous acyclic PDO, not asynchronous/event-driven \(254/255\)
- **read\_only\_string\_capacity:** inferred from the known initial value, including NUL for VISIBLE\_STRING; local\_write remains true, but longer host writes require increasing size
- **exceptions:** Node ID, bitrate, worker period, required nonzero identity, string capacity, unknown types, COB-IDs, mapping structure and unsupported expressions still require input

## Conversion warnings (201)

- \[runtime\] node ID -\> /node\_id: unavailable; supply \{USER\_INPUT\}
- \[DeviceInfo\] selected bitrate \(EDS lists supported rates\) -\> /bitrate\_kbps: unavailable; supply \{USER\_INPUT\}
- \[runtime\] worker period -\> /worker\_period\_us: unavailable; supply \{USER\_INPUT\}
- \[1018sub4\] DefaultValue -\> /identity/serial\_number: unavailable; supply \{USER\_INPUT\}
- \[1007\] DefaultValue -\> /sync/window\_us: unavailable; using default 0
- \[1019\] DefaultValue -\> /sync/counter\_overflow: unavailable; using default 0
- \[1015\] DefaultValue -\> /emcy/inhibit\_100us: unavailable; using default 0
- \[2010suba\] DefaultValue -\> /objects/0/members/10/value: unavailable; using default 0
- \[2010subb\] DefaultValue -\> /objects/0/members/11/value: unavailable; using default 0
- \[2010subc\] DefaultValue -\> /objects/0/members/12/value: unavailable; using default 0
- \[2010subf\] DefaultValue -\> /objects/0/members/15/value: unavailable; using default 0
- \[2010sub10\] DefaultValue -\> /objects/0/members/16/value: unavailable; using default 0
- \[2010sub11\] DefaultValue -\> /objects/0/members/17/value: unavailable; using default 0
- \[2010sub12\] DefaultValue -\> /objects/0/members/18/value: unavailable; using default 0
- \[2010sub13\] DefaultValue -\> /objects/0/members/19/value: unavailable; using default 0
- \[2010sub14\] DefaultValue -\> /objects/0/members/20/value: unavailable; using default 0
- \[2010sub1d\] DefaultValue -\> /objects/0/members/23/value: unavailable; using default 0
- \[2010sub1e\] DefaultValue -\> /objects/0/members/24/value: unavailable; using default 0
- \[2010sub1f\] DefaultValue -\> /objects/0/members/25/value: unavailable; using default 0
- \[2501sub4\] DefaultValue -\> /objects/1/members/1/value: unavailable; using default 0
- \[2501sub5\] DefaultValue -\> /objects/1/members/2/value: unavailable; using default 0
- \[2501sub6\] DefaultValue -\> /objects/1/members/3/value: unavailable; using default 0
- \[2501sub7\] DefaultValue -\> /objects/1/members/4/value: unavailable; using default 0
- \[2501sub8\] DefaultValue -\> /objects/1/members/5/value: unavailable; using default 0
- \[2501sub9\] DefaultValue -\> /objects/1/members/6/value: unavailable; using default 0
- \[2501suba\] DefaultValue -\> /objects/1/members/7/value: unavailable; using default 0
- \[2501subb\] DefaultValue -\> /objects/1/members/8/value: unavailable; using default 0
- \[2501subc\] DefaultValue -\> /objects/1/members/9/value: unavailable; using default 0
- \[2501subd\] DefaultValue -\> /objects/1/members/10/value: unavailable; using default 0
- \[2501\] ARRAY emitted as RECORD to preserve explicit per-subentry metadata and indices
- \[2502suba\] DefaultValue -\> /objects/2/members/10/value: unavailable; using default 0
- \[2502subb\] DefaultValue -\> /objects/2/members/11/value: unavailable; using default 0
- \[2502subc\] DefaultValue -\> /objects/2/members/12/value: unavailable; using default 0
- \[2502subf\] DefaultValue -\> /objects/2/members/15/value: unavailable; using default 0
- \[2502sub10\] DefaultValue -\> /objects/2/members/16/value: unavailable; using default 0
- \[2508sub4\] DefaultValue -\> /objects/3/members/4/value: unavailable; using default 0
- \[2508sub5\] DefaultValue -\> /objects/3/members/5/value: unavailable; using default 0
- \[2508sub7\] DefaultValue -\> /objects/3/members/7/value: unavailable; using default 0
- \[2508subc\] DefaultValue -\> /objects/3/members/11/value: unavailable; using default 0
- \[2508subf\] DefaultValue -\> /objects/3/members/13/value: unavailable; using default 0
- \[2601\] DefaultValue -\> /objects/4/value: unavailable; using default 0
- \[2602\] DefaultValue -\> /objects/5/value: unavailable; using default 0
- \[2680\] DefaultValue -\> /objects/6/value: unavailable; using default 0
- \[2ff0sub1\] DefaultValue -\> /objects/10/members/1/value: unavailable; using default 0
- \[2ff0sub2\] DefaultValue -\> /objects/10/members/2/value: unavailable; using default 0
- \[2ff0sub3\] DefaultValue -\> /objects/10/members/3/value: unavailable; using default 0
- \[3011sub3\] DefaultValue -\> /objects/11/members/3/value: unavailable; using default 0
- \[3011sub4\] DefaultValue -\> /objects/11/members/4/value: unavailable; using default 0
- \[6007\] DefaultValue -\> /objects/13/value: unavailable; using default 0
- \[603f\] DefaultValue -\> /objects/14/value: unavailable; using default 0
- \[6040\] DefaultValue -\> /objects/15/value: unavailable; using default 0
- \[6041\] DefaultValue -\> /objects/16/value: unavailable; using default 0
- \[605b\] DefaultValue -\> /objects/18/value: unavailable; using default 0
- \[605c\] DefaultValue -\> /objects/19/value: unavailable; using default 0
- \[605d\] DefaultValue -\> /objects/20/value: unavailable; using default 0
- \[6060\] DefaultValue -\> /objects/22/value: unavailable; using default 0
- \[6061\] DefaultValue -\> /objects/23/value: unavailable; using default 0
- \[6063\] DefaultValue -\> /objects/24/value: unavailable; using default 0
- \[6064\] DefaultValue -\> /objects/25/value: unavailable; using default 0
- \[6065\] DefaultValue -\> /objects/26/value: unavailable; using default 0
- \[6067\] DefaultValue -\> /objects/27/value: unavailable; using default 0
- \[6068\] DefaultValue -\> /objects/28/value: unavailable; using default 0
- \[606b\] DefaultValue -\> /objects/29/value: unavailable; using default 0
- \[606c\] DefaultValue -\> /objects/30/value: unavailable; using default 0
- \[6072\] DefaultValue -\> /objects/32/value: unavailable; using default 0
- \[6075\] DefaultValue -\> /objects/34/value: unavailable; using default 0
- \[6076\] DefaultValue -\> /objects/35/value: unavailable; using default 0
- \[6077\] DefaultValue -\> /objects/36/value: unavailable; using default 0
- \[6079\] DefaultValue -\> /objects/38/value: unavailable; using default 0
- \[607dsub1\] DefaultValue -\> /objects/41/members/1/value: unavailable; using default 0
- \[607dsub2\] DefaultValue -\> /objects/41/members/2/value: unavailable; using default 0
- \[6080\] DefaultValue -\> /objects/44/value: unavailable; using default 0
- \[6099sub2\] DefaultValue -\> /objects/50/members/2/value: unavailable; using default 0
- \[6099sub3\] DefaultValue -\> /objects/50/members/3/value: unavailable; using default 0
- \[6099sub4\] DefaultValue -\> /objects/50/members/4/value: unavailable; using default 0
- \[6099sub5\] DefaultValue -\> /objects/50/members/5/value: unavailable; using default 0
- \[6099sub6\] DefaultValue -\> /objects/50/members/6/value: unavailable; using default 0
- \[609a\] DefaultValue -\> /objects/51/value: unavailable; using default 0
- \[60c1sub1\] DefaultValue -\> /objects/53/members/1/value: unavailable; using default 0
- \[60c1sub2\] DefaultValue -\> /objects/53/members/2/value: unavailable; using default 0
- \[60c1\] ARRAY emitted as RECORD to preserve explicit per-subentry metadata and indices
- \[60c3\] ARRAY emitted as RECORD to preserve explicit per-subentry metadata and indices
- \[60f4\] DefaultValue -\> /objects/57/value: unavailable; using default 0
- \[60f6sub1\] DefaultValue -\> /objects/58/members/1/value: unavailable; using default 0
- \[60f6sub2\] DefaultValue -\> /objects/58/members/2/value: unavailable; using default 0
- \[60f6sub3\] DefaultValue -\> /objects/58/members/3/value: unavailable; using default 0
- \[60f6sub4\] DefaultValue -\> /objects/58/members/4/value: unavailable; using default 0
- \[60f6sub5\] DefaultValue -\> /objects/58/members/5/value: unavailable; using default 0
- \[60f6sub6\] DefaultValue -\> /objects/58/members/6/value: unavailable; using default 0
- \[60f6sub7\] DefaultValue -\> /objects/58/members/7/value: unavailable; using default 0
- \[60f6sub8\] DefaultValue -\> /objects/58/members/8/value: unavailable; using default 0
- \[60f6sub9\] DefaultValue -\> /objects/58/members/9/value: unavailable; using default 0
- \[60f6suba\] DefaultValue -\> /objects/58/members/10/value: unavailable; using default 0
- \[60f6subb\] DefaultValue -\> /objects/58/members/11/value: unavailable; using default 0
- \[60f6subc\] DefaultValue -\> /objects/58/members/12/value: unavailable; using default 0
- \[60f6subd\] DefaultValue -\> /objects/58/members/13/value: unavailable; using default 0
- \[60f9sub1\] DefaultValue -\> /objects/59/members/1/value: unavailable; using default 0
- \[60f9sub2\] DefaultValue -\> /objects/59/members/2/value: unavailable; using default 0
- \[60f9sub3\] DefaultValue -\> /objects/59/members/3/value: unavailable; using default 0
- \[60f9sub4\] DefaultValue -\> /objects/59/members/4/value: unavailable; using default 0
- \[60f9sub5\] DefaultValue -\> /objects/59/members/5/value: unavailable; using default 0
- \[60f9sub6\] DefaultValue -\> /objects/59/members/6/value: unavailable; using default 0
- \[60f9sub7\] DefaultValue -\> /objects/59/members/7/value: unavailable; using default 0
- \[60f9sub8\] DefaultValue -\> /objects/59/members/8/value: unavailable; using default 0
- \[60f9sub9\] DefaultValue -\> /objects/59/members/9/value: unavailable; using default 0
- \[60f9suba\] DefaultValue -\> /objects/59/members/10/value: unavailable; using default 0
- \[60fbsub1\] DefaultValue -\> /objects/60/members/1/value: unavailable; using default 0
- \[60fbsub2\] DefaultValue -\> /objects/60/members/2/value: unavailable; using default 0
- \[60fbsub3\] DefaultValue -\> /objects/60/members/3/value: unavailable; using default 0
- \[60fbsub4\] DefaultValue -\> /objects/60/members/4/value: unavailable; using default 0
- \[60fbsub5\] DefaultValue -\> /objects/60/members/5/value: unavailable; using default 0
- \[60fc\] DefaultValue -\> /objects/61/value: unavailable; using default 0
- \[60fd\] DefaultValue -\> /objects/62/value: unavailable; using default 0
- \[6410sub1\] DefaultValue -\> /objects/64/members/1/value: unavailable; using default 0
- \[6410sub2\] DefaultValue -\> /objects/64/members/2/value: unavailable; using default 0
- \[6410sub3\] DefaultValue -\> /objects/64/members/3/value: unavailable; using default 0
- \[6410sub4\] DefaultValue -\> /objects/64/members/4/value: unavailable; using default 0
- \[6410sub5\] DefaultValue -\> /objects/64/members/5/value: unavailable; using default 0
- \[6410sub6\] DefaultValue -\> /objects/64/members/6/value: unavailable; using default 0
- \[6410sub7\] DefaultValue -\> /objects/64/members/7/value: unavailable; using default 0
- \[6410sub8\] DefaultValue -\> /objects/64/members/8/value: unavailable; using default 0
- \[6410sub9\] DefaultValue -\> /objects/64/members/9/value: unavailable; using default 0
- \[6410suba\] DefaultValue -\> /objects/64/members/10/value: unavailable; using default 0
- \[6410subb\] DefaultValue -\> /objects/64/members/11/value: unavailable; using default 0
- \[6410subc\] DefaultValue -\> /objects/64/members/12/value: unavailable; using default 0
- \[6410subd\] DefaultValue -\> /objects/64/members/13/value: unavailable; using default 0
- \[6410sube\] DefaultValue -\> /objects/64/members/14/value: unavailable; using default 0
- \[6410subf\] DefaultValue -\> /objects/64/members/15/value: unavailable; using default 0
- \[6410sub10\] DefaultValue -\> /objects/64/members/16/value: unavailable; using default 0
- \[6410sub11\] DefaultValue -\> /objects/64/members/17/value: unavailable; using default 0
- \[6410sub12\] DefaultValue -\> /objects/64/members/18/value: unavailable; using default 0
- \[6410sub13\] DefaultValue -\> /objects/64/members/19/value: unavailable; using default 0
- \[6410sub14\] DefaultValue -\> /objects/64/members/20/value: unavailable; using default 0
- \[6410sub15\] DefaultValue -\> /objects/64/members/21/value: unavailable; using default 0
- \[6410sub16\] DefaultValue -\> /objects/64/members/22/value: unavailable; using default 0
- \[6504\] index=0x6504, subindex=0x00: read-only string capacity inferred as 31 bytes; local writes cannot exceed this fixed capacity
- \[1400sub5\] DefaultValue -\> /rpdos/0/event\_ms: unavailable; using default 0
- \[1400\]/\[1600\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1401sub5\] DefaultValue -\> /rpdos/1/event\_ms: unavailable; using default 0
- \[1401\]/\[1601\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1402sub5\] DefaultValue -\> /rpdos/2/event\_ms: unavailable; using default 0
- \[1602\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1402\]/\[1602\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1403sub5\] DefaultValue -\> /rpdos/3/event\_ms: unavailable; using default 0
- \[1603\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1403\]/\[1603\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1404sub5\] DefaultValue -\> /rpdos/4/event\_ms: unavailable; using default 0
- \[1604\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1404\]/\[1604\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1405sub5\] DefaultValue -\> /rpdos/5/event\_ms: unavailable; using default 0
- \[1605\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1405\]/\[1605\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1406sub5\] DefaultValue -\> /rpdos/6/event\_ms: unavailable; using default 0
- \[1606\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1406\]/\[1606\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1407sub5\] DefaultValue -\> /rpdos/7/event\_ms: unavailable; using default 0
- \[1607\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1407\]/\[1607\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1800sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1800sub2\] DefaultValue -\> /tpdos/0/transmission\_type: unavailable; using default 0
- \[1800\]/\[1a00\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1801sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1801sub2\] DefaultValue -\> /tpdos/1/transmission\_type: unavailable; using default 0
- \[1a01\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1801\]/\[1a01\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1802sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1802sub2\] DefaultValue -\> /tpdos/2/transmission\_type: unavailable; using default 0
- \[1a02\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1802\]/\[1a02\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1803sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1803sub2\] DefaultValue -\> /tpdos/3/transmission\_type: unavailable; using default 0
- \[1a03\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1803\]/\[1a03\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1804sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1804sub2\] DefaultValue -\> /tpdos/4/transmission\_type: unavailable; using default 0
- \[1a04\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1804\]/\[1a04\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1805sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1805sub2\] DefaultValue -\> /tpdos/5/transmission\_type: unavailable; using default 0
- \[1a05\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1805\]/\[1a05\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1806sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1806sub2\] DefaultValue -\> /tpdos/6/transmission\_type: unavailable; using default 0
- \[1a06\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1806\]/\[1a06\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1807sub1\] RTR control is not preserved; simulator does not implement RTR-triggered TPDOs
- \[1807sub2\] DefaultValue -\> /tpdos/7/transmission\_type: unavailable; using default 0
- \[1a07\] empty mapping: PDO emitted disabled despite enabled EDS COB-ID
- \[1807\]/\[1a07\] runtime PDO communication/mapping records are fixed and SDO-read-only
- \[1001\] omitted: outside application range and not a translated communication setting
- \[1008\] omitted: outside application range and not a translated communication setting
- \[1009\] omitted: outside application range and not a translated communication setting
- \[100a\] omitted: outside application range and not a translated communication setting
- \[100b\] omitted: outside application range and not a translated communication setting
- \[100e\] omitted: outside application range and not a translated communication setting
- \[1010\] omitted: outside application range and not a translated communication setting
- \[1011\] omitted: outside application range and not a translated communication setting
- \[1016\] omitted: outside application range and not a translated communication setting
- \[DynamicChannels\] nrofseg='0': not represented in JSON
- EDS metadata/unused definitions not copied \(field counts\): accesstype=249, comments=1, datatype=249, defaultvalue=34, deviceinfo=23, dummyusage=7, fileinfo=11, highlimit=65, lowlimit=67, mandatoryobjects=4, manufacturerobjects=13, objecttype=442, objflags=479, optionalobjects=101, parametername=508, pdomapping=249, subnumber=36; names/limits/flags and supported-feature lists are not runtime validation rules
- EDS describes storage and protocol defaults, not device behavior; CiA 402/vendor behavior still requires a model

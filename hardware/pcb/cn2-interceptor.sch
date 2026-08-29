<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE eagle SYSTEM "eagle.dtd">
<eagle version="9.6.2">
<drawing>
<settings><setting alwaysvectorfont="no"/><setting verticaltext="up"/></settings>
<grid distance="2.54" unitdist="mm" unit="mm" style="lines" multiple="1" display="no" altdistance="0.635" altunitdist="mm" altunit="mm"/>
<layers>
<layer number="1" name="Top" color="4" fill="1" visible="yes" active="yes"/>
<layer number="16" name="Bottom" color="1" fill="1" visible="yes" active="yes"/>
<layer number="17" name="Pads" color="2" fill="1" visible="yes" active="yes"/>
<layer number="18" name="Vias" color="2" fill="1" visible="yes" active="yes"/>
<layer number="19" name="Unrouted" color="6" fill="1" visible="yes" active="yes"/>
<layer number="20" name="Dimension" color="15" fill="1" visible="yes" active="yes"/>
<layer number="21" name="tPlace" color="7" fill="1" visible="yes" active="yes"/>
<layer number="22" name="bPlace" color="7" fill="1" visible="yes" active="yes"/>
<layer number="23" name="tOrigins" color="15" fill="1" visible="yes" active="yes"/>
<layer number="24" name="bOrigins" color="15" fill="1" visible="yes" active="yes"/>
<layer number="25" name="tNames" color="7" fill="1" visible="yes" active="yes"/>
<layer number="26" name="bNames" color="7" fill="1" visible="yes" active="yes"/>
<layer number="27" name="tValues" color="7" fill="1" visible="yes" active="yes"/>
<layer number="28" name="bValues" color="7" fill="1" visible="yes" active="yes"/>
<layer number="29" name="tStop" color="7" fill="3" visible="yes" active="no"/>
<layer number="30" name="bStop" color="7" fill="6" visible="yes" active="no"/>
<layer number="44" name="Drills" color="7" fill="1" visible="yes" active="no"/>
<layer number="45" name="Holes" color="7" fill="1" visible="yes" active="no"/>
<layer number="91" name="Nets" color="2" fill="1" visible="yes" active="yes"/>
<layer number="92" name="Busses" color="1" fill="1" visible="yes" active="yes"/>
<layer number="93" name="Pins" color="2" fill="1" visible="yes" active="no"/>
<layer number="94" name="Symbols" color="4" fill="1" visible="yes" active="yes"/>
<layer number="95" name="Names" color="7" fill="1" visible="yes" active="yes"/>
<layer number="96" name="Values" color="7" fill="1" visible="yes" active="yes"/>
<layer number="97" name="Info" color="7" fill="1" visible="yes" active="yes"/>
<layer number="98" name="Guide" color="6" fill="1" visible="yes" active="yes"/>
</layers>
<schematic xreflabel="%F%N/%S.%C%R" xrefpart="/%S.%C%R">
<libraries>
<library name="cn2interceptor">
<packages>
<package name="JST_XH_4">
<pad name="1" x="0.0000" y="-0.0000" drill="1.00" diameter="1.80" shape="square"/>
<pad name="2" x="2.5400" y="-0.0000" drill="1.00" diameter="1.80" shape="round"/>
<pad name="3" x="5.0800" y="-0.0000" drill="1.00" diameter="1.80" shape="round"/>
<pad name="4" x="7.6200" y="-0.0000" drill="1.00" diameter="1.80" shape="round"/>
<wire x1="-1.300" y1="-1.300" x2="8.920" y2="-1.300" width="0.127" layer="21"/>
<wire x1="8.920" y1="-1.300" x2="8.920" y2="1.300" width="0.127" layer="21"/>
<wire x1="8.920" y1="1.300" x2="-1.300" y2="1.300" width="0.127" layer="21"/>
<wire x1="-1.300" y1="1.300" x2="-1.300" y2="-1.300" width="0.127" layer="21"/>
<text x="-1.300" y="1.700" size="1.016" layer="25">&gt;NAME</text>
</package>
<package name="LS_2X6">
<pad name="LV1" x="0.0000" y="-0.0000" drill="1.00" diameter="1.60" shape="square"/>
<pad name="LV2" x="0.0000" y="-2.5400" drill="1.00" diameter="1.60" shape="round"/>
<pad name="LV" x="0.0000" y="-5.0800" drill="1.00" diameter="1.60" shape="round"/>
<pad name="GND_L" x="0.0000" y="-7.6200" drill="1.00" diameter="1.60" shape="round"/>
<pad name="LV3" x="0.0000" y="-10.1600" drill="1.00" diameter="1.60" shape="round"/>
<pad name="LV4" x="0.0000" y="-12.7000" drill="1.00" diameter="1.60" shape="round"/>
<pad name="HV1" x="10.1600" y="-0.0000" drill="1.00" diameter="1.60" shape="round"/>
<pad name="HV2" x="10.1600" y="-2.5400" drill="1.00" diameter="1.60" shape="round"/>
<pad name="HV" x="10.1600" y="-5.0800" drill="1.00" diameter="1.60" shape="round"/>
<pad name="GND_H" x="10.1600" y="-7.6200" drill="1.00" diameter="1.60" shape="round"/>
<pad name="HV3" x="10.1600" y="-10.1600" drill="1.00" diameter="1.60" shape="round"/>
<pad name="HV4" x="10.1600" y="-12.7000" drill="1.00" diameter="1.60" shape="round"/>
<wire x1="-1.300" y1="-14.000" x2="11.460" y2="-14.000" width="0.127" layer="21"/>
<wire x1="11.460" y1="-14.000" x2="11.460" y2="1.300" width="0.127" layer="21"/>
<wire x1="11.460" y1="1.300" x2="-1.300" y2="1.300" width="0.127" layer="21"/>
<wire x1="-1.300" y1="1.300" x2="-1.300" y2="-14.000" width="0.127" layer="21"/>
<text x="-1.300" y="1.700" size="1.016" layer="25">&gt;NAME</text>
</package>
<package name="XIAO_2X7">
<pad name="D0" x="0.0000" y="-0.0000" drill="1.00" diameter="1.60" shape="square"/>
<pad name="D1" x="0.0000" y="-2.5400" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D2" x="0.0000" y="-5.0800" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D3" x="0.0000" y="-7.6200" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D4" x="0.0000" y="-10.1600" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D5" x="0.0000" y="-12.7000" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D6" x="0.0000" y="-15.2400" drill="1.00" diameter="1.60" shape="round"/>
<pad name="5V" x="15.2400" y="-0.0000" drill="1.00" diameter="1.60" shape="round"/>
<pad name="GND" x="15.2400" y="-2.5400" drill="1.00" diameter="1.60" shape="round"/>
<pad name="3V3" x="15.2400" y="-5.0800" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D10" x="15.2400" y="-7.6200" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D9" x="15.2400" y="-10.1600" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D8" x="15.2400" y="-12.7000" drill="1.00" diameter="1.60" shape="round"/>
<pad name="D7" x="15.2400" y="-15.2400" drill="1.00" diameter="1.60" shape="round"/>
<wire x1="-1.300" y1="-16.540" x2="16.540" y2="-16.540" width="0.127" layer="21"/>
<wire x1="16.540" y1="-16.540" x2="16.540" y2="1.300" width="0.127" layer="21"/>
<wire x1="16.540" y1="1.300" x2="-1.300" y2="1.300" width="0.127" layer="21"/>
<wire x1="-1.300" y1="1.300" x2="-1.300" y2="-16.540" width="0.127" layer="21"/>
<text x="-1.300" y="1.700" size="1.016" layer="25">&gt;NAME</text>
</package>
</packages>
<symbols>
<symbol name="CONN_4">
<wire x1="0" y1="0.00" x2="20.32" y2="0.00" width="0.254" layer="94"/>
<wire x1="0" y1="-12.70" x2="20.32" y2="-12.70" width="0.254" layer="94"/>
<wire x1="0" y1="0.00" x2="0" y2="-12.70" width="0.254" layer="94"/>
<wire x1="20.32" y1="0.00" x2="20.32" y2="-12.70" width="0.254" layer="94"/>
<pin name="1" x="-5.08" y="-2.54" visible="pin" length="middle" direction="pas"/>
<pin name="2" x="-5.08" y="-5.08" visible="pin" length="middle" direction="pas"/>
<pin name="3" x="-5.08" y="-7.62" visible="pin" length="middle" direction="pas"/>
<pin name="4" x="-5.08" y="-10.16" visible="pin" length="middle" direction="pas"/>
<text x="0" y="1.00" size="1.778" layer="95">&gt;NAME</text>
<text x="0" y="-15.10" size="1.778" layer="96">&gt;VALUE</text>
</symbol>
<symbol name="LEVELSHIFT">
<wire x1="0" y1="0.00" x2="20.32" y2="0.00" width="0.254" layer="94"/>
<wire x1="0" y1="-17.78" x2="20.32" y2="-17.78" width="0.254" layer="94"/>
<wire x1="0" y1="0.00" x2="0" y2="-17.78" width="0.254" layer="94"/>
<wire x1="20.32" y1="0.00" x2="20.32" y2="-17.78" width="0.254" layer="94"/>
<pin name="HV1" x="-5.08" y="-2.54" visible="pin" length="middle" direction="pas"/>
<pin name="HV2" x="-5.08" y="-5.08" visible="pin" length="middle" direction="pas"/>
<pin name="HV" x="-5.08" y="-7.62" visible="pin" length="middle" direction="pas"/>
<pin name="GND_H" x="-5.08" y="-10.16" visible="pin" length="middle" direction="pas"/>
<pin name="HV3" x="-5.08" y="-12.70" visible="pin" length="middle" direction="pas"/>
<pin name="HV4" x="-5.08" y="-15.24" visible="pin" length="middle" direction="pas"/>
<pin name="LV1" x="25.40" y="-2.54" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="LV2" x="25.40" y="-5.08" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="LV" x="25.40" y="-7.62" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="GND_L" x="25.40" y="-10.16" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="LV3" x="25.40" y="-12.70" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="LV4" x="25.40" y="-15.24" visible="pin" length="middle" direction="pas" rot="R180"/>
<text x="0" y="1.00" size="1.778" layer="95">&gt;NAME</text>
<text x="0" y="-20.18" size="1.778" layer="96">&gt;VALUE</text>
</symbol>
<symbol name="XIAO_ESP32C3">
<wire x1="0" y1="0.00" x2="20.32" y2="0.00" width="0.254" layer="94"/>
<wire x1="0" y1="-20.32" x2="20.32" y2="-20.32" width="0.254" layer="94"/>
<wire x1="0" y1="0.00" x2="0" y2="-20.32" width="0.254" layer="94"/>
<wire x1="20.32" y1="0.00" x2="20.32" y2="-20.32" width="0.254" layer="94"/>
<pin name="D0" x="-5.08" y="-2.54" visible="pin" length="middle" direction="pas"/>
<pin name="D1" x="-5.08" y="-5.08" visible="pin" length="middle" direction="pas"/>
<pin name="D2" x="-5.08" y="-7.62" visible="pin" length="middle" direction="pas"/>
<pin name="D3" x="-5.08" y="-10.16" visible="pin" length="middle" direction="pas"/>
<pin name="D4" x="-5.08" y="-12.70" visible="pin" length="middle" direction="pas"/>
<pin name="D5" x="-5.08" y="-15.24" visible="pin" length="middle" direction="pas"/>
<pin name="D6" x="-5.08" y="-17.78" visible="pin" length="middle" direction="pas"/>
<pin name="5V" x="25.40" y="-2.54" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="GND" x="25.40" y="-5.08" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="3V3" x="25.40" y="-7.62" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="D10" x="25.40" y="-10.16" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="D9" x="25.40" y="-12.70" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="D8" x="25.40" y="-15.24" visible="pin" length="middle" direction="pas" rot="R180"/>
<pin name="D7" x="25.40" y="-17.78" visible="pin" length="middle" direction="pas" rot="R180"/>
<text x="0" y="1.00" size="1.778" layer="95">&gt;NAME</text>
<text x="0" y="-22.72" size="1.778" layer="96">&gt;VALUE</text>
</symbol>
</symbols>
<devicesets>
<deviceset name="CONN_4" prefix="J">
<gates><gate name="G$1" symbol="CONN_4" x="0" y="0"/></gates>
<devices><device name="" package="JST_XH_4"><connects>
<connect gate="G$1" pin="1" pad="1"/>
<connect gate="G$1" pin="2" pad="2"/>
<connect gate="G$1" pin="3" pad="3"/>
<connect gate="G$1" pin="4" pad="4"/>
</connects><technologies><technology name=""/></technologies></device></devices></deviceset>
<deviceset name="LEVELSHIFT" prefix="U">
<gates><gate name="G$1" symbol="LEVELSHIFT" x="0" y="0"/></gates>
<devices><device name="" package="LS_2X6"><connects>
<connect gate="G$1" pin="HV1" pad="HV1"/>
<connect gate="G$1" pin="HV2" pad="HV2"/>
<connect gate="G$1" pin="HV" pad="HV"/>
<connect gate="G$1" pin="GND_H" pad="GND_H"/>
<connect gate="G$1" pin="HV3" pad="HV3"/>
<connect gate="G$1" pin="HV4" pad="HV4"/>
<connect gate="G$1" pin="LV1" pad="LV1"/>
<connect gate="G$1" pin="LV2" pad="LV2"/>
<connect gate="G$1" pin="LV" pad="LV"/>
<connect gate="G$1" pin="GND_L" pad="GND_L"/>
<connect gate="G$1" pin="LV3" pad="LV3"/>
<connect gate="G$1" pin="LV4" pad="LV4"/>
</connects><technologies><technology name=""/></technologies></device></devices></deviceset>
<deviceset name="XIAO_ESP32C3" prefix="U">
<gates><gate name="G$1" symbol="XIAO_ESP32C3" x="0" y="0"/></gates>
<devices><device name="" package="XIAO_2X7"><connects>
<connect gate="G$1" pin="D0" pad="D0"/>
<connect gate="G$1" pin="D1" pad="D1"/>
<connect gate="G$1" pin="D2" pad="D2"/>
<connect gate="G$1" pin="D3" pad="D3"/>
<connect gate="G$1" pin="D4" pad="D4"/>
<connect gate="G$1" pin="D5" pad="D5"/>
<connect gate="G$1" pin="D6" pad="D6"/>
<connect gate="G$1" pin="5V" pad="5V"/>
<connect gate="G$1" pin="GND" pad="GND"/>
<connect gate="G$1" pin="3V3" pad="3V3"/>
<connect gate="G$1" pin="D10" pad="D10"/>
<connect gate="G$1" pin="D9" pad="D9"/>
<connect gate="G$1" pin="D8" pad="D8"/>
<connect gate="G$1" pin="D7" pad="D7"/>
</connects><technologies><technology name=""/></technologies></device></devices></deviceset>
</devicesets>
</library>
</libraries>
<attributes/><variantdefs/>
<classes><class number="0" name="default" width="0" drill="0"/></classes>
<modules/>
<parts>
<part name="J1" library="cn2interceptor" deviceset="CONN_4" device="" value="JST-XH 4P"/>
<part name="J2" library="cn2interceptor" deviceset="CONN_4" device="" value="JST-XH 4P"/>
<part name="U2" library="cn2interceptor" deviceset="LEVELSHIFT" device="" value="4CH BSS138"/>
<part name="U1" library="cn2interceptor" deviceset="XIAO_ESP32C3" device="" value="XIAO ESP32-C3"/>
</parts>
<sheets><sheet><plain>
<text x="12.7" y="160" size="3.5" layer="97">D8 CN2 INTERCEPTOR</text>
<text x="12.7" y="153" size="2.0" layer="97">CN2 pins 3/4 pass through J1-J2. Pins 1/2 go via U2 to the ESP32.</text>
</plain><instances>
<instance part="J1" gate="G$1" x="20.32" y="127.0"/>
<instance part="J2" gate="G$1" x="20.32" y="88.9"/>
<instance part="U2" gate="G$1" x="76.2" y="116.84"/>
<instance part="U1" gate="G$1" x="137.16" y="116.84"/>
</instances><busses/><nets>
<net name="+5V" class="0">
<segment>
<pinref part="J1" gate="G$1" pin="4"/>
<pinref part="J2" gate="G$1" pin="4"/>
<pinref part="U2" gate="G$1" pin="HV"/>
<pinref part="U1" gate="G$1" pin="5V"/>
<wire x1="15.24" y1="104.78" x2="162.56" y2="104.78" width="0.1524" layer="91"/>
<wire x1="15.24" y1="116.84" x2="15.24" y2="104.78" width="0.1524" layer="91"/>
<wire x1="15.24" y1="78.74" x2="15.24" y2="104.78" width="0.1524" layer="91"/>
<wire x1="71.12" y1="109.22" x2="71.12" y2="104.78" width="0.1524" layer="91"/>
<wire x1="162.56" y1="114.30" x2="162.56" y2="104.78" width="0.1524" layer="91"/>
<label x="15.24" y="105.58" size="1.27" layer="91"/>
</segment></net>
<net name="GND" class="0">
<segment>
<pinref part="J1" gate="G$1" pin="3"/>
<pinref part="J2" gate="G$1" pin="3"/>
<pinref part="U2" gate="G$1" pin="GND_H"/>
<pinref part="U2" gate="G$1" pin="GND_L"/>
<pinref part="U1" gate="G$1" pin="GND"/>
<wire x1="15.24" y1="105.16" x2="162.56" y2="105.16" width="0.1524" layer="91"/>
<wire x1="15.24" y1="119.38" x2="15.24" y2="105.16" width="0.1524" layer="91"/>
<wire x1="15.24" y1="81.28" x2="15.24" y2="105.16" width="0.1524" layer="91"/>
<wire x1="71.12" y1="106.68" x2="71.12" y2="105.16" width="0.1524" layer="91"/>
<wire x1="101.60" y1="106.68" x2="101.60" y2="105.16" width="0.1524" layer="91"/>
<wire x1="162.56" y1="111.76" x2="162.56" y2="105.16" width="0.1524" layer="91"/>
<label x="15.24" y="105.96" size="1.27" layer="91"/>
</segment></net>
<net name="+3V3" class="0">
<segment>
<pinref part="U2" gate="G$1" pin="LV"/>
<pinref part="U1" gate="G$1" pin="3V3"/>
<wire x1="101.60" y1="109.22" x2="162.56" y2="109.22" width="0.1524" layer="91"/>
<label x="101.60" y="110.02" size="1.27" layer="91"/>
</segment></net>
<net name="CH1_HV" class="0">
<segment>
<pinref part="J2" gate="G$1" pin="1"/>
<pinref part="U2" gate="G$1" pin="HV1"/>
<wire x1="15.24" y1="100.33" x2="71.12" y2="100.33" width="0.1524" layer="91"/>
<wire x1="15.24" y1="86.36" x2="15.24" y2="100.33" width="0.1524" layer="91"/>
<wire x1="71.12" y1="114.30" x2="71.12" y2="100.33" width="0.1524" layer="91"/>
<label x="15.24" y="101.13" size="1.27" layer="91"/>
</segment></net>
<net name="CH1_LV" class="0">
<segment>
<pinref part="U2" gate="G$1" pin="LV1"/>
<pinref part="U1" gate="G$1" pin="D1"/>
<wire x1="101.60" y1="113.03" x2="132.08" y2="113.03" width="0.1524" layer="91"/>
<wire x1="101.60" y1="114.30" x2="101.60" y2="113.03" width="0.1524" layer="91"/>
<wire x1="132.08" y1="111.76" x2="132.08" y2="113.03" width="0.1524" layer="91"/>
<label x="101.60" y="113.83" size="1.27" layer="91"/>
</segment></net>
<net name="CH2_HV" class="0">
<segment>
<pinref part="J2" gate="G$1" pin="2"/>
<pinref part="U2" gate="G$1" pin="HV2"/>
<wire x1="15.24" y1="97.79" x2="71.12" y2="97.79" width="0.1524" layer="91"/>
<wire x1="15.24" y1="83.82" x2="15.24" y2="97.79" width="0.1524" layer="91"/>
<wire x1="71.12" y1="111.76" x2="71.12" y2="97.79" width="0.1524" layer="91"/>
<label x="15.24" y="98.59" size="1.27" layer="91"/>
</segment></net>
<net name="CH2_LV" class="0">
<segment>
<pinref part="U2" gate="G$1" pin="LV2"/>
<pinref part="U1" gate="G$1" pin="D2"/>
<wire x1="101.60" y1="110.49" x2="132.08" y2="110.49" width="0.1524" layer="91"/>
<wire x1="101.60" y1="111.76" x2="101.60" y2="110.49" width="0.1524" layer="91"/>
<wire x1="132.08" y1="109.22" x2="132.08" y2="110.49" width="0.1524" layer="91"/>
<label x="101.60" y="111.29" size="1.27" layer="91"/>
</segment></net>
<net name="CH3_HV" class="0">
<segment>
<pinref part="J1" gate="G$1" pin="1"/>
<pinref part="U2" gate="G$1" pin="HV3"/>
<wire x1="15.24" y1="114.30" x2="71.12" y2="114.30" width="0.1524" layer="91"/>
<wire x1="15.24" y1="124.46" x2="15.24" y2="114.30" width="0.1524" layer="91"/>
<wire x1="71.12" y1="104.14" x2="71.12" y2="114.30" width="0.1524" layer="91"/>
<label x="15.24" y="115.10" size="1.27" layer="91"/>
</segment></net>
<net name="CH3_LV" class="0">
<segment>
<pinref part="U2" gate="G$1" pin="LV3"/>
<pinref part="U1" gate="G$1" pin="D3"/>
<wire x1="101.60" y1="105.41" x2="132.08" y2="105.41" width="0.1524" layer="91"/>
<wire x1="101.60" y1="104.14" x2="101.60" y2="105.41" width="0.1524" layer="91"/>
<wire x1="132.08" y1="106.68" x2="132.08" y2="105.41" width="0.1524" layer="91"/>
<label x="101.60" y="106.21" size="1.27" layer="91"/>
</segment></net>
<net name="CH4_HV" class="0">
<segment>
<pinref part="J1" gate="G$1" pin="2"/>
<pinref part="U2" gate="G$1" pin="HV4"/>
<wire x1="15.24" y1="111.76" x2="71.12" y2="111.76" width="0.1524" layer="91"/>
<wire x1="15.24" y1="121.92" x2="15.24" y2="111.76" width="0.1524" layer="91"/>
<wire x1="71.12" y1="101.60" x2="71.12" y2="111.76" width="0.1524" layer="91"/>
<label x="15.24" y="112.56" size="1.27" layer="91"/>
</segment></net>
<net name="CH4_LV" class="0">
<segment>
<pinref part="U2" gate="G$1" pin="LV4"/>
<pinref part="U1" gate="G$1" pin="D4"/>
<wire x1="101.60" y1="102.87" x2="132.08" y2="102.87" width="0.1524" layer="91"/>
<wire x1="101.60" y1="101.60" x2="101.60" y2="102.87" width="0.1524" layer="91"/>
<wire x1="132.08" y1="104.14" x2="132.08" y2="102.87" width="0.1524" layer="91"/>
<label x="101.60" y="103.67" size="1.27" layer="91"/>
</segment></net>
</nets></sheet></sheets></schematic></drawing></eagle>

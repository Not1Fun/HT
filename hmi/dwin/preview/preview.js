"use strict";
(async function () {
  const ui = await fetch("../ui.json", {cache:"no-store"}).then(response => {
    if (!response.ok) throw new Error("无法加载屏幕配置");
    return response.json();
  });
  const $ = selector => document.querySelector(selector);
  const screen = $("#screen");
  const ranges = [1,3,10,30,100,300,1000];
  const frequencies = [2000,5000,8000,10000];
  const events = {boot:1,online:2,offline:3,range:4,frequency:5,batteryOk:6,batteryAlarm:7,
    overcurrent:8,overcurrentClear:9,overvoltage:10,overvoltageClear:11,
    temperatureReady:12,temperatureInvalid:13,temperatureUnavailable:14,ioError:15,
    outputStart:16,outputStop:17,outputFault:18,clear:19,clearFailed:20};
  const names = ["状态页","输出设置页","设备日志页","BENCH 调试页","DAC 独立试波页"];
  const state = {page:0,field:0,draft:7,range:0,rangeChoice:7,frequency:0,power:0,scene:"idle",started:0,
    output:false,matching:false,fault:false,clearing:0,benchDisabled:false,batteryAlarm:false,temperatureInvalid:false,logs:[],logOffset:0};
  const debug = {field:0,draft:0,choice:[0,0,1,0],coils:0,wave:false,until:0};
  function rangeText(index) { return index===7 ? "AUTO" : String(ranges[index]); }
  const amplitudes=[10,25,50,100,1000];
  const relayNames=["OFF","K1","K2 1R","K3 3R","K4 10R","K5 30R","K6 100R","K7 300R","K8 1000R"];
  function stopDebug() {debug.coils=0;debug.wave=false;debug.until=0;if(debug.field===3)debug.draft=0;}
  function debugSelected() {return debug.field===3?Number(debug.wave):debug.choice[debug.field];}
  function debugAllowed() {return !state.fault&&!state.clearing&&state.scene==="idle"&&(!state.temperatureInvalid||state.page===4)&&!state.benchDisabled;}
  function clearFault() {
    if(state.clearing) return;
    stopDebug();state.draft=0;debug.draft=0;
    state.clearing=performance.now()+500;
    announce("演示：正在清除故障并检查新采样，输出保持关闭。");
  }
  function cancelClear() {
    if(!state.clearing) return;
    state.clearing=0;addEvent(events.clearFailed,-125);
    announce("清除已取消，输出保持关闭。");
  }
  const scenes = {idle:0,fault:3,offline:4};
  const bootTime = performance.now();
  function selected() { return state.field===0 ? state.rangeChoice : state.field===1 ? state.frequency : state.field===2 ? state.power : Number(state.output); }
  function dirty() { return state.page===1 && state.draft!==selected(); }
  function announce(text) { $("#feedback").textContent=text; }
  function elapsedSeconds() {
    return state.output ? Math.floor((performance.now()-state.started)/1000) : 0;
  }
  function stopOutput(reason,failed=false) {
    if(!state.output) return false;
    state.output=false;state.matching=false;
    if(failed) state.fault=true;
    if(state.field===3) state.draft=0;
    addEvent(failed?events.outputFault:events.outputStop,failed?-5:0);
    announce(reason);return true;
  }
  function outputState() {
    if(state.clearing) return 6;
    if(state.fault) return 4;
    if(state.benchDisabled) return 5;
    if(state.scene==="offline" || state.power===0 || state.temperatureInvalid) return 3;
    if(state.scene==="fault") return 4;
    if(state.output) return 2;
    return state.page===1 && state.field===3 && state.draft===1 ? 1 : 0;
  }
  function timeText(seconds) {
    if(seconds<0 || seconds>3599999) return "--";
    return [Math.floor(seconds/3600),Math.floor(seconds/60)%60,seconds%60]
      .map(value=>String(value).padStart(2,"0")).join(":");
  }
  function temperatureText(tenths,valid) {
    return valid && Number.isInteger(tenths) && tenths>=-200 && tenths<=1200
      ? (tenths/10).toFixed(1) : "--";
  }
  function lastOffset() { return Math.max(0,state.logs.length-4); }
  function addEvent(kind,value=0) {
    state.logs.unshift({seconds:Math.floor((performance.now()-bootTime)/1000),kind,value});
    if(state.logs.length>32) state.logs.pop();
    if(state.logOffset>0) state.logOffset=Math.min(state.logOffset+1,lastOffset());
  }
  function eventValue(entry) {
    if(entry.kind===events.range) return entry.value+" ohm";
    if(entry.kind===events.frequency || entry.kind===events.outputStart) return entry.value/1000+" kHz";
    if(entry.kind===events.temperatureReady || entry.kind===events.temperatureInvalid) return "NTC"+entry.value;
    if(entry.kind===events.temperatureUnavailable || entry.kind===events.ioError || entry.kind===events.outputFault || entry.kind===events.clearFailed) return String(entry.value);
    return "";
  }
  function image(file,item) {
    const node=document.createElement("img"); node.src="../"+file+"?v=auto12"; node.alt="";
    Object.assign(node.style,{left:item.x+"px",top:item.y+"px",width:item.width+"px",height:item.height+"px"});
    screen.append(node);
  }
  function textValue(field,value) {
    const ns="http://www.w3.org/2000/svg";
    const node=document.createElementNS(ns,"svg");
    const text=document.createElementNS(ns,"text");
    node.classList.add("value"); node.dataset.field=field.name;
    node.setAttribute("width",field.width); node.setAttribute("height",field.height);
    Object.assign(node.style,{left:field.x+"px",top:field.y+"px",color:field.color,fontSize:field.font_height+"px"});
    text.setAttribute("x","0"); text.setAttribute("y",Math.round(field.font_height*.82));
    text.setAttribute("fill","currentColor"); text.setAttribute("lengthAdjust","spacingAndGlyphs");
    text.setAttribute("textLength",value.length*field.font_width);
    text.textContent=value; node.append(text); screen.append(node);
  }
  function render() {
    screen.replaceChildren(); screen.dataset.page=String(state.page);
    screen.dataset.scene=state.scene; screen.dataset.dirty=String(dirty());screen.dataset.output=String(state.output);
    screen.dataset.matching=String(state.matching);
    screen.setAttribute("aria-label",names[state.page]+"，演示数据，未连接设备");
    image(ui.pages[state.page].image,{x:0,y:0,width:800,height:480});
    const online=state.scene!=="offline";
    const values={state:state.fault?3:state.matching?2:state.output?1:scenes[state.scene],battery:online?(state.batteryAlarm?1:0):2,
      focus:state.field,edit:dirty()?1:0,output:outputState()};
    values.debug_focus=debug.field;
    values.dac_focus=debug.field-1;
    values.debug_state=state.clearing?5:state.fault?4:!debugAllowed()?3:debug.wave?2:debug.until?1:0;
    values.dac_state=values.debug_state;
    const reasons=[];
    if(state.scene==="offline") reasons.push(5);
    if(state.benchDisabled) reasons.push(1);
    if(state.scene==="fault") reasons.push(11,12);
    else if(state.fault) reasons.push(26);
    if(state.temperatureInvalid && state.page!==4 && !(state.page===3 && debug.choice[0]===0)) reasons.push(18);
    if(state.page===1 && state.power===0) reasons.push(29);
    values.reason=reasons.length?reasons[Math.floor(performance.now()/2000)%reasons.length]:0;
    if(state.clearing) values.reason=39;
    const choiceFrequency=state.field===1?state.draft:state.frequency;
    const running=state.output&&!state.matching;
    const texts={range_choice:rangeText(state.field===0?state.draft:state.rangeChoice),current:running?Math.sqrt(state.power/ranges[state.range]).toFixed(3):"--",voltage:running?Math.sqrt(state.power*ranges[state.range]).toFixed(3):"--",range:state.output?String(ranges[state.range]):"--",
      power_choice:String(state.field===2?state.draft:state.power),
      frequency:state.output?String(frequencies[state.frequency]/1000):"--",
      elapsed:running?timeText(elapsedSeconds()):"--",
      frequency_choice:String(frequencies[choiceFrequency]/1000),
      ntc1:temperatureText(250,online),ntc2:temperatureText(314,online&&!state.temperatureInvalid),
      ntc3:temperatureText(-52,online)};
    Object.assign(texts,{
      debug_relay:relayNames[debug.field===0?debug.draft:debug.choice[0]],
      debug_frequency:String(frequencies[debug.field===1?debug.draft:debug.choice[1]]/1000),
      debug_amplitude:String(amplitudes[debug.field===2?debug.draft:debug.choice[2]]),
      debug_wave:debug.wave?"ON":debug.field===3&&debug.draft?"START?":"OFF",
      debug_coils:debug.coils.toString(2).padStart(8,"0"),
      debug_seconds:String(Math.max(0,Math.ceil((debug.until-performance.now())/1000))),
      debug_voltage:"--",debug_current:"--"
    });
    for(let row=0;row<4;row++) {
      const entry=state.logs[state.logOffset+row];
      values["log_event_"+row]=entry?entry.kind:0;
      texts["log_time_"+row]=entry?timeText(entry.seconds):"";
      texts["log_value_"+row]=entry?eventValue(entry):"";
    }
    const start=state.logs.length?state.logOffset+1:0;
    const end=Math.min(state.logOffset+4,state.logs.length);
    texts.log_position=String(start).padStart(2,"0")+"-"+String(end).padStart(2,"0")+"/"+String(state.logs.length).padStart(2,"0");
    for(const item of ui.icons.filter(item=>item.pages.includes(state.page))) {
      image("assets/icons/"+String(item.first+values[item.name]).padStart(3,"0")+".png",item);
    }
    for(const field of ui.fields.filter(field=>field.pages.includes(state.page))) textValue(field,texts[field.name]);
    $("#page-label").textContent=names[state.page];
    $("#ccw").setAttribute("aria-label",state.page===2?"左旋：较新日志":state.field===3?"左旋：预选关闭":state.field===2?"左旋：减小目标 VA":state.field===0?"左旋：上一个阻抗挡位":"左旋：上一个频率挡位");
    $("#cw").setAttribute("aria-label",state.page===2?"右旋：较旧日志":state.field===3?"右旋：预选开启":state.field===2?"右旋：增大目标 VA":state.field===0?"右旋：下一个阻抗挡位":"右旋：下一个频率挡位");
    $("#press").setAttribute("aria-label",state.page===2?"编码器下压返回最新日志":"编码器下压确认");
    document.querySelectorAll("[data-scene]").forEach(button=>button.setAttribute("aria-pressed",String(button.dataset.scene===state.scene)));
  }
  function moveLog(delta) {
    state.logOffset=Math.max(0,Math.min(lastOffset(),state.logOffset+delta));
    announce(state.logOffset===0?"演示日志：当前为最新记录。":"演示日志：正在查看较早记录，按 OK 返回最新。");
  }
  function key(key) {
    if(key==="left" || key==="right") {
      const abandoned=dirty();
      const before=state.page;
      state.page=(state.page+(key==="right"?1:names.length-1))%names.length;
      state.draft=selected();
      if(before>=3 || state.page>=3) {
        cancelClear();
        if(debug.choice[2]>3) debug.choice[2]=0;
        stopOutput("进入调试已停止恒 VA 演示。");stopDebug();debug.field=0;debug.draft=0;debug.choice[0]=0;
        if(state.page===4) {debug.field=1;debug.choice[1]=debug.choice[2]=0;}
      }
      if(state.page===2) state.logOffset=0;
      announce((abandoned?"未确认修改已取消。":"")+"已切到"+names[state.page]+"。");
    } else if(state.page===0) {
      if(key==="ok" || key==="press") {
        if(state.fault) state.field=3;
        state.page=1; state.draft=selected(); announce("上下选择阻抗、频率、目标 VA 或输出，旋钮预选后按下确认。");
      }
      if(key==="down") {cancelClear();if(!stopOutput("已立即停止演示输出。")) announce("输出当前已关闭。");}
    } else if(state.page>=3) {
      if(key==="up" || key==="down") {
        debug.field=Math.max(state.page===4?1:0,Math.min(3,debug.field+(key==="down"?1:-1)));debug.draft=debugSelected();
      } else if(key==="ok" || key==="press") {
        const choice=debug.draft;
        debug.choice[debug.field]=choice;
        if(debug.field===3 && (state.fault||state.clearing)) clearFault();
        else if(debug.field===1 || debug.field===2) {cancelClear();stopDebug();debug.draft=choice;}
        else if(debug.field===3 && (debug.wave || !choice) || debug.field===0 && !choice) stopDebug();
        else if(debugAllowed()) {
          debug.wave=debug.field===3;
          const relay=state.page===4?0:debug.choice[0];
          debug.coils=relay<2?relay:(1<<(relay-1))|(relay>=6?1:0);
          debug.until=performance.now()+(debug.wave?10000:30000);
        }
      }
      announce("仅演示控制命令；实机 DAC/继电器动作以烧录后的设备为准。");
    } else if(state.page===2) {
      if(key==="up" || key==="down") moveLog(key==="down"?1:-1);
      else if(key==="ok" || key==="press") {state.logOffset=0;announce("演示日志已返回最新记录。");}
    } else if(key==="up" || key==="down") {
      const field=Math.max(0,Math.min(3,state.field+(key==="up"?-1:1)));
      if(field!==state.field) {state.field=field;state.draft=selected();}
      announce("已选中"+["阻抗挡位","频率挡位","目标 VA","输出"][state.field]+"，旋转调整后按下确认。");
    } else if(key==="ok" || key==="press") {
      if(state.field===3) {
        if(state.fault||state.clearing) clearFault();
        else if(state.output) stopOutput("已停止演示输出；再次启动需重新右旋并确认。");
        else if(outputState()>=3) {state.draft=0;announce("当前状态不能启动输出；恢复可用后重新预选并确认。");}
        else if(state.draft===1) {
          state.output=true;state.matching=state.rangeChoice===7;state.started=performance.now();state.range=state.matching?0:state.rangeChoice;
          if(!state.matching) addEvent(events.range,ranges[state.range]);
          addEvent(events.outputStart,frequencies[state.frequency]);
          announce(state.matching?"正在演示自动选挡，模拟负载为30 Ω；频率保持用户选择。":"已按手动阻抗挡位开始恒 VA 演示输出；再次确认或状态页下键立即停止。");
        } else announce("输出保持关闭，右旋预选开启后按下确认。");
      } else {
        cancelClear();
        const stopped=stopOutput("确认新请求前已停止演示输出。");
        if(state.field===1) {state.frequency=state.draft;addEvent(events.frequency,frequencies[state.frequency]);}
        else if(state.field===0) state.rangeChoice=state.draft;
        else state.power=state.draft;
        announce((stopped?"输出已停止。":"")+"演示请求已保存："+(state.field===0?rangeText(state.rangeChoice):state.field===1?frequencies[state.frequency]/1000+" kHz":state.power+" VA")+"。输出需重新开启并确认。");
      }
    }
    render();
  }
  function turn(delta) {
    if(state.page===0) {announce("在设置页旋转选择挡位，在日志页旋转翻看记录。");return;}
    if(state.page>=3) {
      const count=debug.field===0?9:debug.field===3?2:debug.field===2&&state.page===4?5:4;
      debug.draft=debug.field===3?Math.max(0,Math.min(1,debug.draft+delta)):(debug.draft+delta+count)%count;
      announce("待确认；按 OK / 下压执行，左右切页停止。");
    } else if(state.page===2) moveLog(delta);
    else {
      if(state.field===3) state.draft=Math.max(0,Math.min(1,state.draft+delta));
      else if(state.field===2) state.draft=Math.max(0,Math.min(50,state.draft+delta));
      else {
        const count=state.field===0?ranges.length+1:frequencies.length;
        state.draft=(state.draft+delta+count)%count;
      }
      announce(state.field===3?(state.output?"正在演示输出；按下编码器或 OK 将立即停止。":state.draft?"已预选开启，按下编码器或 OK 才会按手动挡位输出。":"已预选关闭。"):dirty()?"当前为预选值，按下编码器或 OK 保存请求。":"与已确认请求一致。");
    }
    render();
  }
  for(const name of ["up","left","ok","right","down","press"]) $("#"+name).addEventListener("click",()=>key(name));
  $("#cw").addEventListener("click",()=>turn(1));
  $("#ccw").addEventListener("click",()=>turn(-1));
  document.querySelectorAll("[data-scene]").forEach(button=>button.addEventListener("click",()=>{
    const next=button.dataset.scene;
    if(next===state.scene) return;
    if(next!=="idle") stopDebug();
    if(next!=="idle") stopOutput("演示状态变化，输出已停止。",next==="fault");
    if(state.scene==="offline") addEvent(events.online);
    if(state.scene==="fault") addEvent(events.overcurrentClear);
    if(next==="offline") addEvent(events.offline);
    if(next==="fault") {state.fault=true;addEvent(events.overcurrent);}
    state.scene=next;announce("已切换演示状态；温度、事件和值均为模拟数据。");render();
  }));
  $("#battery-alarm").addEventListener("change",event=>{
    state.batteryAlarm=event.target.checked;
    addEvent(state.batteryAlarm?events.batteryAlarm:events.batteryOk);
    announce("已切换电池演示状态，并添加一条模拟日志。");render();
  });
  $("#temperature-invalid").addEventListener("change",event=>{
    state.temperatureInvalid=event.target.checked;
    if(state.temperatureInvalid) stopDebug();
    if(state.temperatureInvalid) stopOutput("温度数据无效，演示输出已停止。",true);
    addEvent(state.temperatureInvalid?events.temperatureInvalid:events.temperatureReady,2);
    announce("已切换 NTC2 演示状态，并添加一条模拟日志。");render();
  });
  $("#bench-disabled").addEventListener("change",event=>{
    state.benchDisabled=event.target.checked;
    if(state.benchDisabled) stopDebug();
    if(state.benchDisabled) stopOutput("输出功能未启用，输出已停止。");
    state.draft=selected();
    announce(state.benchDisabled?"模拟普通固件：未启用输出功能，不能启动输出。":"模拟输出功能已启用：仍默认关闭，需要重新预选并确认。");render();
  });
  function resize() {screen.style.transform=`scale(${$(".screen-stage").clientWidth/800})`;}
  new ResizeObserver(resize).observe($(".screen-stage"));
  // These seed records demonstrate the page and do not come from connected hardware.
  addEvent(events.boot);addEvent(events.online);addEvent(events.batteryOk);
  for(let channel=1;channel<=3;channel++) addEvent(events.temperatureReady,channel);
  render();resize();
  let reasonSlot=-1;
  setInterval(()=>{
    if(state.output&&state.matching&&performance.now()-state.started>=800) {
      state.matching=false;state.range=3;addEvent(events.range,30);render();
    }
    if(state.clearing && performance.now()>=state.clearing) {
      state.clearing=0;
      if(state.scene!=="idle"||state.benchDisabled) {
        addEvent(events.clearFailed,-13);announce("清除未完成：保护输入仍有效，具体原因见下方。");
      } else {
        state.fault=false;state.draft=debug.draft=0;debug.choice[3]=0;
        addEvent(events.clear);announce("故障已清除，输出保持关闭；启动需重新右旋并确认。");
      }
      render();
    }
    const slot=Math.floor(performance.now()/2000);
    if(slot!==reasonSlot) {reasonSlot=slot;if(state.page===1||state.page>=3)render();}
    if(debug.until) {
      if(performance.now()>=debug.until) stopDebug();
      render();
    }
    const field=screen.querySelector('[data-field="elapsed"] text');
    if(field && state.output && !state.matching) {
      field.textContent=timeText(elapsedSeconds());
      field.setAttribute("textLength",field.textContent.length*ui.fields.find(item=>item.name==="elapsed").font_width);
    }
  },250);
})().catch(error=>{document.querySelector("#feedback").textContent=error.message;console.error(error);});

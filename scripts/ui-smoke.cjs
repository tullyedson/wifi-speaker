// Test the actual native app through its own WebView2 debugging endpoint.
const { chromium } = require('../.tools/ui-qa/node_modules/playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const net = require('node:net');
const { spawn } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function reserve() { const server = net.createServer(); await new Promise(resolve => server.listen(0,'127.0.0.1',resolve)); return server; }
async function port() { const server = await reserve(); const value = server.address().port; await new Promise(resolve => server.close(resolve)); return value; }
async function main() {
  const directory = path.join(root, 'runtime', 'ui-' + Date.now());
  await fs.mkdir(directory, { recursive:true });
  const hubPort = await port(), debugPort = await port();
  const base = `http://127.0.0.1:${hubPort}`;
  await fs.writeFile(path.join(directory,'settings.json'),JSON.stringify({listen_url:base,public_url:base,whisper_model:path.join(root,'models','ggml-base.en.bin')}));
  const process = spawn(path.join(root,'target','release','smart-speaker.exe'),['--tray'],{cwd:root,windowsHide:true,stdio:'ignore',env:{...global.process.env,SMART_SPEAKER_CONFIG_DIR:directory,WEBVIEW2_USER_DATA_FOLDER:path.join(directory,'webview'),WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS:`--remote-debugging-port=${debugPort}`}});
  let browser;
  const report = {success:false,checks:[],screenshots:[]};
  try {
    for (let tries=0; ; tries++) {
      if(process.exitCode!==null) throw new Error('Native desktop exited during startup');
      try { const response=await fetch(`http://127.0.0.1:${debugPort}/json/version`); if(response.ok) break; } catch {}
      if(tries>=100) throw new Error('WebView2 debug endpoint did not start'); await delay(200);
    }
    browser = await chromium.connectOverCDP(`http://127.0.0.1:${debugPort}`);
    const context = browser.contexts()[0];
    let page;
    for(let tries=0;tries<50;tries++) { page=context.pages()[0]; if(page) break; await delay(100); }
    if(!page) throw new Error('Native settings WebView not found');
    page.setDefaultTimeout(10000);
    const errors=[]; page.on('pageerror',error=>errors.push(error.message));
    await page.locator('#hub-badge').filter({hasText:'Hub running'}).waitFor();
    assert.equal(await page.title(),'Smart Speaker');
    assert.ok(await page.locator('#voice_id option').count()>1);
    assert.equal(await page.locator('#model-state').textContent(),'Ready');
    report.checks.push('Native startup, listener, installed voices and model detection');
    await page.locator('#add-speaker').click();
    let cards = page.locator('.speaker-card');
    await cards.nth(0).getByLabel('Speaker name').fill('Kitchen');
    await cards.nth(0).getByLabel('Tags, separated by commas').fill('downstairs, kitchen');
    await page.locator('#add-speaker').click();
    await cards.nth(1).getByLabel('Speaker name').fill('Office');
    await cards.nth(1).getByLabel('Tags, separated by commas').fill('upstairs, work');
    const setRange = async (card, label, value) => card.getByRole('slider', {name:label}).evaluate((node, next) => { node.value = next; node.dispatchEvent(new Event('input',{bubbles:true})); }, String(value));
    await setRange(cards.nth(0), 'Microphone gain', 4);
    await setRange(cards.nth(0), 'Playback volume', 80);
    await setRange(cards.nth(1), 'Microphone gain', 0.5);
    await setRange(cards.nth(1), 'Playback volume', 20);
    await page.locator('#save').click();
    await page.locator('#save-state').filter({hasText:'All changes saved'}).waitFor();
    const getSettings = () => page.evaluate(()=>window.__TAURI__.core.invoke('get_settings'));
    const saved = await getSettings();
    assert.deepEqual(saved.speakers.map(s=>s.name),['Kitchen','Office']);
    assert.deepEqual(saved.speakers[0].tags,['downstairs','kitchen']);
    const onDisk=JSON.parse(await fs.readFile(path.join(directory,'settings.json'),'utf8'));
    assert.deepEqual(onDisk.speakers,saved.speakers);
    assert.deepEqual(saved.speakers.map(s=>[s.microphone_gain,s.volume]), [[4,80],[0.5,20]]);
    await page.reload();
    await page.locator('.speaker-card').nth(1).waitFor();
    cards = page.locator('.speaker-card');
    assert.equal(await cards.nth(0).getByRole('slider',{name:'Microphone gain'}).inputValue(),'4');
    assert.equal(await cards.nth(1).getByRole('slider',{name:'Playback volume'}).inputValue(),'20');
    report.checks.push('Independent per-speaker gain and volume save, reload and remain distinct');
    report.checks.push('Add, name and tag two speakers; save through Rust IPC to disk');
    await cards.nth(0).getByText('Setup details').click();
    assert.equal(await page.locator('#setup-id').inputValue(),saved.speakers[0].id);
    assert.equal(await page.locator('#setup-token').inputValue(),saved.speakers[0].token);
    await page.locator('#setup-dialog summary').click();
    await page.locator('#wifi-ssid').fill('verification-only');
    await page.locator('#wifi-password').fill('not-a-real-password');
    await page.evaluate(()=>Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async text=>{window.testClipboard=text;}}}));
    await page.locator('#copy-provision').click();
    const provisioning=JSON.parse(await page.evaluate(()=>window.testClipboard));
    assert.equal(provisioning.type,'provision'); assert.equal(provisioning.wifi_ssid,'verification-only'); assert.equal(provisioning.hub_url,base);
    assert.equal(provisioning.speaker_id,saved.speakers[0].id); assert.equal(provisioning.speaker_token,saved.speakers[0].token);
    await page.locator('.dialog-close').click();
    await page.locator('#setup-dialog').waitFor({state:'hidden'});
    await page.waitForFunction(()=>document.getElementById('wifi-password').value==='');
    report.checks.push('Setup dialog produces the firmware provisioning contract and clears Wi-Fi credentials');
    await page.locator('[data-page="connection"]').click();
    assert.equal(await page.locator('#callback-url').inputValue(),base+'/v1/responses');
    await page.locator('#listen_url').fill('http://not-an-ip:48490');
    await page.locator('#save').click();
    await page.locator('#notice.error').waitFor();
    assert.equal((await getSettings()).listen_url,base);
    const occupied=await reserve();
    try {
      await page.locator('#listen_url').fill('http://127.0.0.1:'+occupied.address().port);
      await page.locator('#save').click();
      await page.locator('#notice').filter({hasText:'previous settings restored'}).waitFor();
      assert.equal((await getSettings()).listen_url,base);
      assert.equal((await fetch(base+'/healthz')).status,200);
    } finally { occupied.close(); }
    await page.locator('#listen_url').fill(base); await page.locator('#save').click();
    await page.locator('#save-state').filter({hasText:'All changes saved'}).waitFor();
    report.checks.push('Invalid settings rejected; occupied-port save restores saved configuration and listener');
    await page.locator('#pause').click(); await page.locator('#hub-badge').filter({hasText:'Listening paused'}).waitFor();
    await page.locator('#pause').click(); await page.locator('#hub-badge').filter({hasText:'Hub running'}).waitFor();
    report.checks.push('Pause and resume use actual Rust state');
    await page.locator('#backend_mode').selectOption('open_ai');
    await page.locator('#openai-fields').waitFor({state:'visible'});
    await page.locator('#model').fill('verification-model');
    await page.locator('#openai-fields details summary').click();
    await page.locator('#router_instance_id').fill('fixture-router-instance');
    await page.locator('#router_header_name').fill('X-Client-Instance');
    await page.locator('#model_max_tokens').fill('2048');
    await page.locator('#save').click(); await page.locator('#save-state').filter({hasText:'All changes saved'}).waitFor();
    assert.equal((await getSettings()).router_instance_id,'fixture-router-instance');
    assert.equal((await getSettings()).model_max_tokens,2048);
    assert.equal((await getSettings()).router_header_name,'X-Client-Instance');
    assert.equal(JSON.parse(await fs.readFile(path.join(directory,'settings.json'),'utf8')).router_instance_id,'fixture-router-instance');
    await page.reload();
    await page.locator('#hub-badge').filter({hasText:'Hub running'}).waitFor();
    await page.locator('[data-page="connection"]').click();
    assert.equal(await page.locator('#router_header_name').inputValue(),'X-Client-Instance');
    await page.locator('#openai-fields details summary').click();
    report.checks.push('OpenAI response budget, custom caller header and hub ID persist and reload');
    await page.screenshot({path:path.join(directory,'openai.png'),fullPage:true});
    report.screenshots.push(path.join(directory,'openai.png'));
    await page.locator('#backend_mode').selectOption('webhook');
    await page.locator('#save').click(); await page.locator('#save-state').filter({hasText:'All changes saved'}).waitFor();
    for(const section of ['speakers','connection','speech','guide']) {
      await page.locator(`[data-page="${section}"]`).click();
      await page.screenshot({path:path.join(directory,section+'.png'),fullPage:true});
      report.screenshots.push(path.join(directory,section+'.png'));
    }
    await page.setViewportSize({width:720,height:540});
    await page.locator('[data-page="connection"]').click();
    assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    await page.screenshot({path:path.join(directory,'compact.png'),fullPage:true});
    assert.deepEqual(errors,[]);
    report.checks.push('All settings sections render, compact layout fits, no JavaScript errors');
    report.success=true;
  } finally {
    if(browser) await browser.close();
    process.kill();
    await fs.writeFile(path.join(directory,'report.json'),JSON.stringify(report,null,2));
    console.log(JSON.stringify({...report,report:path.join(directory,'report.json')},null,2));
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});

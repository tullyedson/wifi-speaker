#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use smart_speaker_hub::{
    config::{new_token, ConfigStore, Settings, SpeakerConfig},
    engine::Hub,
    protocol::{Announcement, HubStatus},
    server::{self, Listener},
    speech::{self, LocalSpeech, Voice},
};
use std::{path::PathBuf, sync::Arc};
use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    Manager,
};
use tauri_plugin_autostart::ManagerExt;
use tokio::sync::Mutex;

struct AppState {
    hub: Arc<Hub>,
    store: ConfigStore,
    listener: Mutex<Option<Listener>>,
    models: PathBuf,
}
type CommandResult<T> = Result<T, String>;

#[tauri::command]
fn get_settings(state: tauri::State<'_, AppState>) -> Settings {
    state.hub.settings()
}
#[tauri::command]
async fn get_status(state: tauri::State<'_, AppState>) -> CommandResult<HubStatus> {
    Ok(state.hub.status().await)
}
#[tauri::command]
async fn save_settings(state: tauri::State<'_, AppState>, settings: Settings) -> CommandResult<()> {
    settings.validate().map_err(|e| e.to_string())?;
    let mut listener = state.listener.lock().await;
    let previous = state.hub.settings();
    if let Some(old) = listener.take() {
        old.stop(&state.hub).await;
    }
    if let Err(error) = state.store.save(&settings) {
        *listener = server::start(state.hub.clone()).await.ok();
        return Err(error.to_string());
    }
    state
        .hub
        .replace_settings(settings)
        .await
        .map_err(|e| e.to_string())?;
    match server::start(state.hub.clone()).await {
        Ok(new_listener) => {
            *listener = Some(new_listener);
            Ok(())
        }
        Err(error) => {
            let _ = state.store.save(&previous);
            let _ = state.hub.replace_settings(previous).await;
            *listener = server::start(state.hub.clone()).await.ok();
            Err(format!(
                "Listener could not start; previous settings restored: {error}"
            ))
        }
    }
}
#[tauri::command]
async fn set_paused(state: tauri::State<'_, AppState>, paused: bool) -> CommandResult<()> {
    state.hub.set_paused(paused).await;
    Ok(())
}
#[tauri::command]
fn new_speaker() -> SpeakerConfig {
    SpeakerConfig {
        id: format!(
            "speaker-{}",
            &uuid::Uuid::new_v4().simple().to_string()[..12]
        ),
        name: "New speaker".into(),
        tags: vec![],
        token: new_token(),
        enabled: true,
        microphone_gain: 1.0,
        volume: 65,
    }
}
#[tauri::command]
async fn get_voices() -> CommandResult<Vec<Voice>> {
    tokio::task::spawn_blocking(speech::voices)
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| e.to_string())
}
#[tauri::command]
async fn download_model(state: tauri::State<'_, AppState>) -> CommandResult<String> {
    speech::download_model(&state.models)
        .await
        .map(|p| p.to_string_lossy().into_owned())
        .map_err(|e| e.to_string())
}
#[tauri::command]
async fn test_speaker(state: tauri::State<'_, AppState>, speaker_id: String) -> CommandResult<()> {
    state
        .hub
        .announce(Announcement {
            request_id: uuid::Uuid::new_v4(),
            speaker_ids: vec![speaker_id],
            tags: vec![],
            text: "Your smart speaker is connected and ready.".into(),
        })
        .await
        .map(|_| ())
        .map_err(|e| e.to_string())
}
#[tauri::command]
fn startup_enabled(app: tauri::AppHandle) -> CommandResult<bool> {
    app.autolaunch().is_enabled().map_err(|e| e.to_string())
}
#[tauri::command]
fn set_startup(app: tauri::AppHandle, enabled: bool) -> CommandResult<()> {
    if enabled {
        app.autolaunch().enable()
    } else {
        app.autolaunch().disable()
    }
    .map_err(|e| e.to_string())
}

fn show_settings(app: &tauri::AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
    }
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _, _| {
            show_settings(app)
        }))
        .plugin(
            tauri_plugin_autostart::Builder::new()
                .args(["--tray"])
                .build(),
        )
        .invoke_handler(tauri::generate_handler![
            get_settings,
            get_status,
            save_settings,
            set_paused,
            new_speaker,
            get_voices,
            download_model,
            test_speaker,
            startup_enabled,
            set_startup
        ])
        .setup(|app| {
            let directory = match std::env::var_os("SMART_SPEAKER_CONFIG_DIR") {
                Some(path) => PathBuf::from(path),
                None => app.path().app_config_dir()?,
            };
            let store = ConfigStore::new(directory.join("settings.json"));
            let mut settings = store.load()?;
            let bundled_model = std::env::current_exe()?
                .parent()
                .unwrap()
                .join("models/ggml-base.en.bin");
            if settings.whisper_model.is_empty() && bundled_model.is_file() {
                settings.whisper_model = bundled_model.to_string_lossy().into_owned();
                store.save(&settings)?;
            }
            let hub = Hub::new(settings, Arc::new(LocalSpeech::default()))?;
            let listener = tauri::async_runtime::block_on(server::start(hub.clone()));
            if let Err(error) = &listener {
                eprintln!("Listener is unavailable: {error}");
            }
            app.manage(AppState {
                hub,
                store,
                listener: Mutex::new(listener.ok()),
                models: directory.join("models"),
            });
            let settings_item = MenuItem::with_id(app, "settings", "Settings", true, None::<&str>)?;
            let pause_item =
                MenuItem::with_id(app, "pause", "Pause / resume listening", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "Exit", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&settings_item, &pause_item, &quit_item])?;
            let mut pixels = vec![0_u8; 32 * 32 * 4];
            for y in 0..32_usize {
                for x in 0..32_usize {
                    let index = (y * 32 + x) * 4;
                    let bar = (9..=13).contains(&x) && (6..=22).contains(&y)
                        || (17..=21).contains(&x) && (10..=26).contains(&y);
                    if bar {
                        pixels[index..index + 4].copy_from_slice(&[90, 225, 189, 255]);
                    }
                }
            }
            TrayIconBuilder::with_id("smart-speaker")
                .icon(tauri::image::Image::new_owned(pixels, 32, 32))
                .tooltip("Smart Speaker")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "settings" => show_settings(app),
                    "pause" => {
                        let hub = app.state::<AppState>().hub.clone();
                        tauri::async_runtime::spawn(async move {
                            hub.set_paused(!hub.paused()).await;
                        });
                    }
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if matches!(
                        event,
                        TrayIconEvent::Click {
                            button: MouseButton::Left,
                            button_state: MouseButtonState::Up,
                            ..
                        }
                    ) {
                        show_settings(tray.app_handle());
                    }
                })
                .build(app)?;
            if !std::env::args().any(|a| a == "--tray") {
                show_settings(app.handle());
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .build(tauri::generate_context!())
        .expect("Cannot start Smart Speaker")
        .run(|app, event| {
            if matches!(event, tauri::RunEvent::Exit) {
                let state = app.state::<AppState>();
                tauri::async_runtime::block_on(async {
                    if let Some(listener) = state.listener.lock().await.take() {
                        listener.stop(&state.hub).await;
                    }
                });
            }
        });
}

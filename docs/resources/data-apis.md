# Live data APIs

Public sources of live data that a piece reads with `web-in`, `web-stream`, or `web-socket`, without an account or a key. Each entry gives the endpoint, what it carries, and a call that reads it. Every endpoint answers JSON, and a field is a JSON Pointer into it ([Web data](../language.md#web-data)). Each provider's documentation states its terms and limits; a piece polls no faster than its data changes.

| API | Carries | Primitive | Example |
|---|---|---|---|
| [Open-Meteo](#open-meteo) | Current weather anywhere | `web-in` | `weather` |
| [USGS earthquakes](#usgs-earthquakes) | Earthquakes of the past hour | `web-in` | |
| [ISS position](#iss-position) | The International Space Station's latitude and longitude | `web-in` | |
| [Wikimedia recent changes](#wikimedia-recent-changes) | Every edit to a Wikimedia wiki, as it happens | `web-stream` | `edits` |
| [Bluesky Jetstream](#bluesky-jetstream) | Every public Bluesky post, as it happens | `web-socket` | `firehose` |

## Open-Meteo

Current conditions for a latitude and longitude: temperature, wind, precipitation, cloud cover, and more, named in the URL's `current` list. Documentation: <https://open-meteo.com/en/docs>.

```lisp
(define forecast
  "https://api.open-meteo.com/v1/forecast?latitude=41.88&longitude=-87.63&current=temperature_2m,wind_speed_10m")
(web-in :url forecast :every 60s :fields ["/current/temperature_2m" "/current/wind_speed_10m"] :value weather)
```

`weather` is a control pipe of two channels: degrees Celsius and kilometres an hour.

## USGS earthquakes

The United States Geological Survey's feed of every earthquake recorded in the past hour, as GeoJSON, regenerated every minute. Other feeds cover the past day, week, and month, and larger magnitudes only. Documentation: <https://earthquake.usgs.gov/earthquakes/feed/v1.0/geojson.php>.

```lisp
(web-in :url "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/all_hour.geojson"
        :every 60s :fields ["/metadata/count" "/features/0/properties/mag"] :value quakes)
```

`quakes` holds the number of earthquakes in the past hour and the magnitude of the latest.

## ISS position

The International Space Station's position over the Earth. The endpoint answers over plain HTTP only, and gives the coordinates as strings that hold numbers, which `web-in` reads as numbers. Documentation: <http://open-notify.org/Open-Notify-API/ISS-Location-Now/>.

```lisp
(web-in :url "http://api.open-notify.org/iss-now.json"
        :every 5s :fields ["/iss_position/latitude" "/iss_position/longitude"] :value station)
```

`station` holds the latitude, −90 to 90, and the longitude, −180 to 180, in degrees.

## Wikimedia recent changes

A stream of server-sent events, one for each change to a page of any Wikimedia wiki, several each second. Wikimedia asks every client to name itself in a `User-Agent` header. Documentation: <https://wikitech.wikimedia.org/wiki/Event_Platform/EventStreams>.

```lisp
(web-stream :url "https://stream.wikimedia.org/v2/stream/recentchange"
            :headers ["User-Agent: my-piece"]
            :fields ["/length/new"]
            :value size
            :out edits)
```

Each change sends an event on `edits`, and `size` holds the page's new length in bytes.

## Bluesky Jetstream

A WebSocket of the Bluesky network's public activity, filtered by the collections the URL names; `app.bsky.feed.post` is posts. Documentation: <https://github.com/bluesky-social/jetstream>.

```lisp
(web-socket :url "wss://jetstream2.us-east.bsky.network/subscribe?wantedCollections=app.bsky.feed.post"
            :fields ["/time_us"]
            :out posts)
```

Each post sends an event on `posts`.

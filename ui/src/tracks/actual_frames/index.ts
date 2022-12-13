// Copyright (C) 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import { PluginContext } from '../../common/plugin_api';
import { NUM, NUM_NULL, STR } from '../../common/query_result';
import { fromPs, toPs } from '../../common/time';
import { TrackData } from '../../common/track_data';
import { TrackController } from '../../controller/track_controller';
import { NewTrackArgs, Track } from '../../frontend/track';
import { ChromeSliceTrack } from '../chrome_slices';

export const ACTUAL_FRAMES_SLICE_TRACK_KIND = 'ActualFramesSliceTrack';

export interface Config {
  maxDepth: number;
  trackIds: number[];
}

export interface Data extends TrackData {
  // Slices are stored in a columnar fashion. All fields have the same length.
  strings: string[];
  sliceIds: Float64Array;
  starts: Float64Array;
  ends: Float64Array;
  depths: Uint16Array;
  titles: Uint16Array;   // Index in |strings|.
  colors?: Uint16Array;  // Index in |strings|.
  isInstant: Uint16Array;
  isIncomplete: Uint16Array;
}

const BLUE_COLOR = '#03A9F4';         // Blue 500
const GREEN_COLOR = '#4CAF50';        // Green 500
const YELLOW_COLOR = '#FFEB3B';       // Yellow 500
const RED_COLOR = '#FF5722';          // Red 500
const LIGHT_GREEN_COLOR = '#C0D588';  // Light Green 500
const PINK_COLOR = '#F515E0';         // Pink 500

class ActualFramesSliceTrackController extends TrackController<Config, Data> {
  static readonly kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
  private maxDurPs = 0;

  async onBoundsChange(start: number, end: number, resolution: number):
    Promise<Data> {
    const startPs = toPs(start);
    const endPs = toPs(end);

    const pxSize = this.pxSize();

    // ns per quantization bucket (i.e. ns per pixel). /2 * 2 is to force it to
    // be an even number, so we can snap in the middle.
    const bucketPs = Math.max(Math.round(resolution * 1e12 * pxSize / 2) * 2, 1);

    if (this.maxDurPs === 0) {
      const maxDurResult = await this.query(`
        select
          max(iif(dur = -1, (SELECT end_ts FROM trace_bounds) - ts, dur))
            as maxDur
        from experimental_slice_layout
        where filter_track_ids = '${this.config.trackIds.join(',')}'
      `);
      this.maxDurPs = maxDurResult.firstRow({ maxDur: NUM_NULL }).maxDur || 0;
    }

    const rawResult = await this.query(`
      SELECT
        (s.ts + ${bucketPs / 2}) / ${bucketPs} * ${bucketPs} as tsq,
        s.ts as ts,
        max(iif(s.dur = -1, (SELECT end_ts FROM trace_bounds) - s.ts, s.dur))
            as dur,
        s.layout_depth as layoutDepth,
        s.name as name,
        s.id as id,
        s.dur = 0 as isInstant,
        s.dur = -1 as isIncomplete,
        CASE afs.jank_tag
          WHEN 'Self Jank' THEN '${RED_COLOR}'
          WHEN 'Other Jank' THEN '${YELLOW_COLOR}'
          WHEN 'Dropped Frame' THEN '${BLUE_COLOR}'
          WHEN 'Buffer Stuffing' THEN '${LIGHT_GREEN_COLOR}'
          WHEN 'SurfaceFlinger Stuffing' THEN '${LIGHT_GREEN_COLOR}'
          WHEN 'No Jank' THEN '${GREEN_COLOR}'
          ELSE '${PINK_COLOR}'
        END as color
      from experimental_slice_layout s
      join actual_frame_timeline_slice afs using(id)
      where
        filter_track_ids = '${this.config.trackIds.join(',')}' and
        s.ts >= ${startPs - this.maxDurPs} and
        s.ts <= ${endPs}
      group by tsq, s.layout_depth
      order by tsq, s.layout_depth
    `);

    const numRows = rawResult.numRows();
    const slices: Data = {
      start,
      end,
      resolution,
      length: numRows,
      strings: [],
      sliceIds: new Float64Array(numRows),
      starts: new Float64Array(numRows),
      ends: new Float64Array(numRows),
      depths: new Uint16Array(numRows),
      titles: new Uint16Array(numRows),
      colors: new Uint16Array(numRows),
      isInstant: new Uint16Array(numRows),
      isIncomplete: new Uint16Array(numRows),
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = slices.strings.length;
      slices.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    const it = rawResult.iter({
      'tsq': NUM,
      'ts': NUM,
      'dur': NUM,
      'layoutDepth': NUM,
      'id': NUM,
      'name': STR,
      'isInstant': NUM,
      'isIncomplete': NUM,
      'color': STR,
    });
    for (let i = 0; it.valid(); i++, it.next()) {
      const startPsQ = it.tsq;
      const startPs = it.ts;
      const durPs = it.dur;
      const endPs = startPs + durPs;

      let endPsQ = Math.floor((endPs + bucketPs / 2 - 1) / bucketPs) * bucketPs;
      endPsQ = Math.max(endPsQ, startPsQ + bucketPs);

      slices.starts[i] = fromPs(startPsQ);
      slices.ends[i] = fromPs(endPsQ);
      slices.depths[i] = it.layoutDepth;
      slices.titles[i] = internString(it.name);
      slices.colors![i] = internString(it.color);
      slices.sliceIds[i] = it.id;
      slices.isInstant[i] = it.isInstant;
      slices.isIncomplete[i] = it.isIncomplete;
    }
    return slices;
  }
}

export class ActualFramesSliceTrack extends ChromeSliceTrack {
  static readonly kind = ACTUAL_FRAMES_SLICE_TRACK_KIND;
  static create(args: NewTrackArgs): Track {
    return new ActualFramesSliceTrack(args);
  }
}

export function activate(ctx: PluginContext) {
  ctx.registerTrackController(ActualFramesSliceTrackController);
  ctx.registerTrack(ActualFramesSliceTrack);
}

export const plugin = {
  pluginId: 'perfetto.ActualFrames',
  activate,
};

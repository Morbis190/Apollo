<script setup>
import { ref, onMounted } from 'vue'
import Checkbox from '../../Checkbox.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
const seats = ref([])
const loading = ref(false)

async function fetchSeats() {
  loading.value = true
  try {
    const r = await fetch('./api/seats', { credentials: 'include' })
    const data = await r.json()
    if (data.status) {
      seats.value = data.seats || []
    }
  } catch (e) {
    console.error('Failed to fetch seats:', e)
  } finally {
    loading.value = false
  }
}

async function releaseSeat(seatId) {
  if (!confirm(`Force release seat "${seatId}"?`)) return
  try {
    const r = await fetch('./api/seats/release', {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: seatId }),
    })
    const data = await r.json()
    if (data.status) {
      await fetchSeats()
    }
  } catch (e) {
    console.error('Failed to release seat:', e)
  }
}

onMounted(() => {
  fetchSeats()
})
</script>

<template>
  <div id="multiseat" class="config-page">
    <!-- Enable Multi-Seat -->
    <div class="mb-3">
      <label for="multiseat_enabled" class="form-label">Enable Multi-Seat</label>
      <select id="multiseat_enabled" class="form-select" v-model="config.multiseat_enabled">
        <option value="enabled">Enabled</option>
        <option value="disabled">Disabled</option>
      </select>
      <div class="form-text">
        When enabled, multiple clients can stream simultaneously, each with their own isolated display, audio, and input.
      </div>
    </div>

    <!-- Max Seats -->
    <div class="mb-3">
      <label for="max_seats" class="form-label">Maximum Seats</label>
      <input type="number" class="form-control" id="max_seats" v-model="config.max_seats" min="1" max="16" />
      <div class="form-text">
        Maximum number of concurrent streaming sessions (1-16).
      </div>
    </div>

    <!-- Auto Virtual Display -->
    <div class="mb-3">
      <label for="multiseat_auto_virtual_display" class="form-label">Auto Virtual Display</label>
      <select id="multiseat_auto_virtual_display" class="form-select" v-model="config.multiseat_auto_virtual_display">
        <option value="enabled">Enabled</option>
        <option value="disabled">Disabled</option>
      </select>
      <div class="form-text">
        Automatically create a virtual display for each seat.
      </div>
    </div>

    <!-- Session Isolation (Windows only) -->
    <div class="mb-3" v-if="platform === 'windows'">
      <label for="multiseat_session_isolation" class="form-label">Session Isolation</label>
      <select id="multiseat_session_isolation" class="form-select" v-model="config.multiseat_session_isolation">
        <option value="enabled">Enabled</option>
        <option value="disabled">Disabled</option>
      </select>
      <div class="form-text">
        Isolate each seat into its own Windows Desktop so processes are invisible to other seats.
      </div>
    </div>

    <!-- Active Seats -->
    <div class="mt-4">
      <h5>Active Seats
        <button class="btn btn-sm btn-outline-secondary ms-2" @click="fetchSeats" :disabled="loading">
          {{ loading ? 'Loading...' : 'Refresh' }}
        </button>
      </h5>
      <div v-if="seats.length === 0" class="text-muted">No seats allocated.</div>
      <table class="table table-sm" v-else>
        <thead>
          <tr>
            <th>ID</th>
            <th>State</th>
            <th>Display</th>
            <th>Audio</th>
            <th>Actions</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="seat in seats" :key="seat.id">
            <td>{{ seat.id }}</td>
            <td>
              <span class="badge" :class="seat.state === 'bound' ? 'bg-success' : 'bg-secondary'">
                {{ seat.state }}
              </span>
            </td>
            <td>{{ seat.display_name || '(default)' }}</td>
            <td>{{ seat.audio_sink_id || '(default)' }}</td>
            <td>
              <button class="btn btn-sm btn-outline-danger" v-if="seat.state === 'bound'" @click="releaseSeat(seat.id)">
                Release
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

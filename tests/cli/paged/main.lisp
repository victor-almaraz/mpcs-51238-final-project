(defpiece knob ()
  (osc-in :port :page :address "/cutoff" :value (pipe :control)))
